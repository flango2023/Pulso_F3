// ============================================================================
// Pulso Band - Firmware ESP32 (Fase 3 - CardioIA / Projeto Pulso)
// ============================================================================
//
// Projeto academico - Graduacao Tecnologica em Inteligencia Artificial - FIAP
// Autor: Richard Schmitz | RM 567951 | Turma 2TIAOR-2026
//
// OBJETIVO
//   Simular um dispositivo vestivel (wearable) de monitoramento cardiaco que:
//     1. Le sinais vitais de 2 sensores distintos (DHT22 obrigatorio + batimentos)
//     2. Processa e armazena os dados localmente (Edge Computing)
//     3. Garante resiliencia offline com um buffer limitado em RAM
//     4. Transmite os dados para a nuvem via MQTT quando ha conexao Wi-Fi
//     5. Dispara alertas quando valores ultrapassam limites clinicos
//
// SENSORES
//   - DHT22 (GPIO 15): temperatura (C) e umidade (%)  -> sensor obrigatorio
//   - Botao  (GPIO 4): simula batimentos cardiacos. Cada pressao = 1 batida.
//                      O firmware converte a frequencia de pressoes em bpm.
//
// NOTA SOBRE O SPIFFS
//   O enunciado explica que o SPIFFS e volatil em simuladores (Wokwi/PlatformIO)
//   e so persiste em chip real. Por isso, a resiliencia offline aqui e feita por
//   um buffer circular em RAM e o "envio para a nuvem" offline e representado
//   pelo Monitor Serial, conforme sugerido pelo proprio enunciado.
//
// BROKER MQTT
//   Broker publico do HiveMQ (broker.hivemq.com, porta 1883, sem TLS e sem
//   autenticacao). Escolhido para o ambiente academico por nao exigir conta nem
//   servidor local, sendo acessivel diretamente pelo Wokwi. A ausencia de TLS e
//   de autenticacao e uma limitacao de seguranca documentada na governanca.
// ============================================================================

#include <WiFi.h>              // Wi-Fi do ESP32 (usado pela stack de rede)
#include <WiFiClient.h>        // Conexao TCP simples com o broker (porta 1883)
#include <PubSubClient.h>      // Cliente MQTT
#include <DHT.h>               // Driver do sensor DHT22
#include <ArduinoJson.h>       // Monta o payload JSON enviado por MQTT

// ----------------------------------------------------------------------------
// 1. CONFIGURACAO DE PINOS E SENSORES
// ----------------------------------------------------------------------------
#define PINO_DHT     15        // DHT22 ligado ao GPIO 15
#define TIPO_DHT     DHT22
#define PINO_BOTAO   4         // Botao (batimentos) ligado ao GPIO 4
#define PINO_LED     2         // LED onboard: acende em caso de alerta

DHT dht(PINO_DHT, TIPO_DHT);   // Instancia o driver do DHT22

// ----------------------------------------------------------------------------
// 2. CONFIGURACAO DE REDE E MQTT
// ----------------------------------------------------------------------------
// No Wokwi, a rede "Wokwi-GUEST" (sem senha) da acesso a internet simulada.
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// Broker publico do HiveMQ: nao exige conta, usuario nem senha.
const char* MQTT_HOST = "broker.hivemq.com";
const int   MQTT_PORT = 1883;       // porta TCP padrao MQTT (sem TLS)

// Topicos MQTT. O prefixo inclui o RM para evitar colisao com outros usuarios
// do broker publico, que e compartilhado por qualquer pessoa na internet.
const char* TOPICO_TELEMETRIA = "pulso/band/rm567951/telemetria"; // dados dos sensores
const char* TOPICO_ALERTA     = "pulso/band/rm567951/alerta";     // eventos de alerta

WiFiClient clienteRede;             // Socket TCP simples
PubSubClient mqtt(clienteRede);     // Cliente MQTT sobre o socket TCP

// ----------------------------------------------------------------------------
// 3. LIMITES CLINICOS DE ALERTA
// ----------------------------------------------------------------------------
// Limites justificados clinicamente (ver relatorio Parte 2):
//   - Taquicardia em repouso: FC > 100 bpm (American Heart Association)
//   - Febre: temperatura corporal > 37,8 C
// Observacao: no wearable, a temperatura do DHT22 representa a temperatura de
// pele/ambiente do dispositivo, nao a temperatura central do paciente. O limite
// e didatico para demonstrar a logica de alerta.
const float LIMITE_BPM  = 100.0;
const float LIMITE_TEMP = 37.8;

// ----------------------------------------------------------------------------
// 4. RESILIENCIA OFFLINE - BUFFER CIRCULAR EM RAM
// ----------------------------------------------------------------------------
// Estrategia alinhada ao modelo de negocio do Pulso Band (ver relatorio Parte 1):
// o dispositivo e um wearable de uso continuo. Em perda de conexao, ele precisa
// continuar coletando sem travar nem estourar a memoria.
//
// Dimensionamento (calculo real, documentado no relatorio):
//   - Cada amostra ocupa ~48 bytes na struct Leitura.
//   - Reservamos um buffer de 200 amostras = ~9,6 KB de RAM.
//   - Com 1 amostra a cada 5 s, 200 amostras cobrem ~16,6 minutos offline.
//   - O ESP32 tem ~320 KB de RAM livre; 9,6 KB e uma fracao segura.
// Quando o buffer enche, aplicamos politica FIFO: a leitura mais antiga e
// descartada para dar lugar a mais recente (dado recente tem mais valor clinico).
#define TAMANHO_BUFFER 200

struct Leitura {
  unsigned long timestamp;  // millis() no momento da coleta
  float temperatura;        // C
  float umidade;            // %
  int   bpm;                // batimentos por minuto estimados
  bool  alerta;             // true se ultrapassou algum limite
};

Leitura buffer[TAMANHO_BUFFER]; // armazenamento circular
int inicioBuffer = 0;           // indice da leitura mais antiga
int totalBuffer  = 0;           // quantidade de leituras armazenadas

// ----------------------------------------------------------------------------
// 5. SIMULACAO DE CONECTIVIDADE
// ----------------------------------------------------------------------------
// Variavel booleana que simula a disponibilidade de rede, conforme o enunciado.
// Alternamos o estado periodicamente para demonstrar o ciclo offline/online.
bool conectado = true;
unsigned long ultimaTrocaConexao = 0;
const unsigned long INTERVALO_TROCA_CONEXAO = 30000; // alterna a cada 30 s

// ----------------------------------------------------------------------------
// 6. CONTAGEM DE BATIMENTOS (BOTAO)
// ----------------------------------------------------------------------------
// Cada pressao do botao conta como 1 batida. A cada janela de coleta, convertemos
// o numero de batidas na janela para bpm (batidas por minuto).
volatile unsigned long contagemBatidas = 0;
unsigned long ultimaBatida = 0;
const unsigned long DEBOUNCE_MS = 120; // evita contagem dupla por repique

unsigned long ultimaColeta = 0;
const unsigned long INTERVALO_COLETA = 5000; // coleta a cada 5 s

// ----------------------------------------------------------------------------
// 7. INTERRUPCAO DO BOTAO
// ----------------------------------------------------------------------------
// ISR (Interrupt Service Routine): incrementa a contagem de batidas a cada
// borda de descida do botao, com debounce por software.
void IRAM_ATTR aoApertarBotao() {
  unsigned long agora = millis();
  if (agora - ultimaBatida > DEBOUNCE_MS) {
    contagemBatidas++;
    ultimaBatida = agora;
  }
}

// ----------------------------------------------------------------------------
// 8. FUNCOES DO BUFFER CIRCULAR
// ----------------------------------------------------------------------------
// Insere uma leitura no buffer. Se estiver cheio, descarta a mais antiga (FIFO).
void bufferInserir(Leitura l) {
  int pos = (inicioBuffer + totalBuffer) % TAMANHO_BUFFER;
  if (totalBuffer < TAMANHO_BUFFER) {
    buffer[pos] = l;
    totalBuffer++;
  } else {
    // Buffer cheio: sobrescreve a posicao mais antiga e avanca o inicio.
    buffer[inicioBuffer] = l;
    inicioBuffer = (inicioBuffer + 1) % TAMANHO_BUFFER;
  }
}

// Remove e retorna a leitura mais antiga do buffer.
Leitura bufferRemover() {
  Leitura l = buffer[inicioBuffer];
  inicioBuffer = (inicioBuffer + 1) % TAMANHO_BUFFER;
  totalBuffer--;
  return l;
}

// ----------------------------------------------------------------------------
// 9. CONEXAO WI-FI E MQTT
// ----------------------------------------------------------------------------
void conectarWiFi() {
  Serial.print("[WiFi] Conectando a ");
  Serial.print(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
    delay(300);
    Serial.print(".");
    tentativas++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" conectado.");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" falhou (seguindo em modo offline simulado).");
  }
}

void conectarMQTT() {
  // Conexao ao broker publico do HiveMQ via TCP simples (sem TLS/autenticacao).
  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  int tentativas = 0;
  while (!mqtt.connected() && tentativas < 3) {
    Serial.print("[MQTT] Conectando ao broker publico HiveMQ... ");
    // ID de cliente aleatorio evita conflito com outros clientes no broker publico.
    String idCliente = "PulsoBand-rm567951-" + String(random(0xffff), HEX);
    if (mqtt.connect(idCliente.c_str())) {
      Serial.println("conectado.");
    } else {
      Serial.print("falhou, rc=");
      Serial.print(mqtt.state());
      Serial.println(" (nova tentativa em 2 s)");
      delay(2000);
      tentativas++;
    }
  }
}

// ----------------------------------------------------------------------------
// 10. ENVIO DE UMA LEITURA PARA A NUVEM
// ----------------------------------------------------------------------------
// Monta o payload JSON e publica no topico de telemetria. Se a leitura tiver
// alerta, publica tambem no topico de alerta. Retorna true se o publish teve
// sucesso (usado pela sincronizacao para decidir se limpa o buffer).
bool enviarLeitura(Leitura l) {
  StaticJsonDocument<256> doc;
  doc["timestamp"]   = l.timestamp;
  doc["temperatura"] = l.temperatura;
  doc["umidade"]     = l.umidade;
  doc["bpm"]         = l.bpm;
  doc["alerta"]      = l.alerta;

  char payload[256];
  serializeJson(doc, payload);

  bool ok = mqtt.publish(TOPICO_TELEMETRIA, payload);

  if (l.alerta) {
    // Mensagem de alerta separada, consumida pelo Node-RED para acender o LED
    // virtual e exibir o aviso no dashboard.
    StaticJsonDocument<128> alerta;
    alerta["bpm"]         = l.bpm;
    alerta["temperatura"] = l.temperatura;
    alerta["mensagem"]    = "Valores fora dos limites clinicos";
    char payloadAlerta[128];
    serializeJson(alerta, payloadAlerta);
    mqtt.publish(TOPICO_ALERTA, payloadAlerta);
  }

  return ok;
}

// ----------------------------------------------------------------------------
// 11. SINCRONIZACAO - NUCLEO DA RESILIENCIA OFFLINE
// ----------------------------------------------------------------------------
// Quando a conexao volta, esvazia o buffer enviando as leituras acumuladas na
// ordem de coleta (FIFO) e so remove cada leitura do buffer apos envio com
// sucesso. Isso garante que nenhum dado se perca em caso de falha no meio do
// processo (equivalente a "apagar o arquivo local apos enviar").
void sincronizarBuffer() {
  if (totalBuffer == 0) return;

  Serial.print("[SYNC] Conexao restabelecida. Sincronizando ");
  Serial.print(totalBuffer);
  Serial.println(" leitura(s) acumulada(s)...");

  while (totalBuffer > 0) {
    Leitura l = buffer[inicioBuffer]; // le sem remover ainda
    if (enviarLeitura(l)) {
      bufferRemover();                // remove apenas apos envio confirmado
      Serial.println("[SYNC] Leitura sincronizada e removida do buffer local.");
    } else {
      Serial.println("[SYNC] Falha no envio. Mantendo no buffer para nova tentativa.");
      break;                          // interrompe; tenta de novo no proximo ciclo
    }
    delay(50);
  }

  if (totalBuffer == 0) {
    Serial.println("[SYNC] Buffer local vazio. Todos os dados foram sincronizados.");
  }
}

// ----------------------------------------------------------------------------
// 12. COLETA DE UMA AMOSTRA DOS SENSORES
// ----------------------------------------------------------------------------
Leitura coletarLeitura() {
  Leitura l;
  l.timestamp = millis();

  // --- DHT22: temperatura e umidade ---
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  // Se a leitura falhar (NaN), reaproveita valores plausiveis para nao travar.
  l.temperatura = isnan(t) ? 36.5 : t;
  l.umidade     = isnan(h) ? 50.0 : h;

  // --- Botao: converte batidas da janela em bpm ---
  // bpm = batidas na janela * (60000 ms / duracao da janela em ms)
  unsigned long batidas = contagemBatidas;
  contagemBatidas = 0; // zera o contador para a proxima janela
  l.bpm = (int)(batidas * (60000.0 / INTERVALO_COLETA));

  // --- Regra de alerta clinico ---
  l.alerta = (l.bpm > LIMITE_BPM) || (l.temperatura > LIMITE_TEMP);

  return l;
}

// ----------------------------------------------------------------------------
// 13. SETUP
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("====================================================");
  Serial.println("  Pulso Band - Monitoramento Cardiaco Continuo (IoT)");
  Serial.println("  Fase 3 - CardioIA / Projeto Pulso");
  Serial.println("====================================================");

  dht.begin();
  pinMode(PINO_LED, OUTPUT);
  pinMode(PINO_BOTAO, INPUT_PULLUP);
  // Interrupcao na borda de descida (botao com pull-up: pressionar = nivel baixo)
  attachInterrupt(digitalPinToInterrupt(PINO_BOTAO), aoApertarBotao, FALLING);

  conectarWiFi();
  conectarMQTT();

  ultimaTrocaConexao = millis();
  ultimaColeta = millis();
}

// ----------------------------------------------------------------------------
// 14. LOOP PRINCIPAL
// ----------------------------------------------------------------------------
void loop() {
  unsigned long agora = millis();

  // Mantem a conexao MQTT viva.
  if (mqtt.connected()) mqtt.loop();

  // --- Simula troca de estado da conexao a cada 30 s ---
  if (agora - ultimaTrocaConexao >= INTERVALO_TROCA_CONEXAO) {
    conectado = !conectado;
    ultimaTrocaConexao = agora;
    Serial.println();
    Serial.print(">>> Mudanca de conectividade: agora ");
    Serial.println(conectado ? "ONLINE" : "OFFLINE");
    // Ao voltar a ficar online, sincroniza imediatamente o que ficou no buffer.
    if (conectado) {
      if (!mqtt.connected()) conectarMQTT();
      sincronizarBuffer();
    }
  }

  // --- Coleta periodica dos sensores ---
  if (agora - ultimaColeta >= INTERVALO_COLETA) {
    ultimaColeta = agora;
    Leitura l = coletarLeitura();

    // LED fisico acende localmente quando ha alerta (feedback no proprio device).
    digitalWrite(PINO_LED, l.alerta ? HIGH : LOW);

    // Log legivel no Monitor Serial (serve tambem como "nuvem offline").
    Serial.println("----------------------------------------------------");
    Serial.print("[COLETA] t=");   Serial.print(l.temperatura, 1); Serial.print(" C  ");
    Serial.print("umid=");         Serial.print(l.umidade, 0);     Serial.print(" %  ");
    Serial.print("bpm=");          Serial.print(l.bpm);
    Serial.print("  alerta=");     Serial.println(l.alerta ? "SIM" : "nao");

    if (conectado) {
      // Online: garante MQTT ativo, sincroniza pendencias e envia a leitura atual.
      if (!mqtt.connected()) conectarMQTT();
      sincronizarBuffer();
      if (enviarLeitura(l)) {
        Serial.println("[CLOUD] Leitura publicada via MQTT no HiveMQ.");
      } else {
        Serial.println("[CLOUD] Falha no publish. Armazenando no buffer local.");
        bufferInserir(l);
      }
    } else {
      // Offline: armazena localmente (Edge). O Monitor Serial evidencia o acumulo.
      bufferInserir(l);
      Serial.print("[EDGE] Offline. Leitura armazenada no buffer local (");
      Serial.print(totalBuffer);
      Serial.print("/");
      Serial.print(TAMANHO_BUFFER);
      Serial.println(" amostras).");
    }
  }
}
