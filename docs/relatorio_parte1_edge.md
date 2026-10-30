# Relatório Parte 1 — Armazenamento e processamento local (Edge Computing)

**Projeto:** Pulso — Inteligência Cardiológica com Consciência de Dados
**Fase 3:** Monitoramento Contínuo — IoT na Saúde
**Dispositivo:** Pulso Band (wearable cardíaco simulado)
**Autor:** Richard Schmitz — RM 567951 — Turma 2TIAOR-2026

---

## 1. Visão geral do fluxo de funcionamento

O Pulso Band é um dispositivo vestível simulado em ESP32 (Wokwi) que monitora
sinais vitais de forma contínua. O fluxo de dados segue o ciclo completo de uma
aplicação de IoT em saúde:

```
captura → processamento local → armazenamento → transmissão → visualização → alerta
```

Nesta Parte 1, o foco está nas três primeiras etapas, que ocorrem na borda da
rede (Edge Computing), ou seja, no próprio dispositivo, antes de qualquer envio
para a nuvem.

### Sensores

| Sensor | Pino | Grandeza medida | Papel |
|---|---|---|---|
| DHT22 | GPIO 15 | Temperatura (°C) e umidade (%) | Sensor obrigatório |
| Botão de pressão | GPIO 4 | Batimentos cardíacos (bpm) | Segundo sensor — sinal vital principal |
| LED onboard | GPIO 2 | — | Feedback visual de alerta no próprio dispositivo |

O botão simula o pulso: cada pressão representa uma batida. A cada janela de
coleta de 5 segundos, o firmware conta as batidas e converte para batimentos por
minuto pela fórmula `bpm = batidas × (60000 / 5000)`.

### Ciclo de coleta

A cada 5 segundos o dispositivo:

1. Lê temperatura e umidade do DHT22.
2. Converte a contagem de batidas do botão em bpm.
3. Avalia se os valores ultrapassam os limites clínicos de alerta.
4. Decide, com base no estado de conexão, se envia para a nuvem ou armazena
   localmente.

---

## 2. Limites clínicos de alerta

Os limites não são arbitrários. Foram definidos a partir de referências clínicas
reconhecidas, coerentes com o eixo de governança do projeto:

| Parâmetro | Limite | Justificativa |
|---|---|---|
| Frequência cardíaca | > 100 bpm | Taquicardia em repouso (American Heart Association) |
| Temperatura | > 37,8 °C | Limiar de febre (didático — ver limitação abaixo) |

**Limitação registrada:** no wearable, o DHT22 mede a temperatura de pele/ambiente
do dispositivo, não a temperatura corporal central. O limite de 37,8 °C é
didático e serve para demonstrar a lógica de alerta, não para diagnóstico real.

---

## 3. Lógica de resiliência offline

### O problema

Um wearable cardíaco de uso contínuo não pode parar de coletar dados quando perde
a conexão. Em um cenário real — um paciente no metrô, em área rural, ou com o
celular distante — a rede cai com frequência. Perder leituras nesses momentos é
inaceitável em uma aplicação de saúde, porque é justamente nos deslocamentos e
fora de casa que muitos eventos cardíacos ocorrem.

### A estratégia adotada: buffer circular em RAM

O enunciado esclarece que o SPIFFS é volátil em simuladores e só persiste em chip
real. Por isso, a resiliência offline foi implementada com um **buffer circular
em RAM**, e o "envio para a nuvem" em modo offline é evidenciado pelo **Monitor
Serial**, conforme a alternativa sugerida pelo próprio enunciado.

O funcionamento:

- Quando a conexão está **ativa**, cada leitura é publicada imediatamente via MQTT
  e qualquer pendência acumulada é sincronizada antes.
- Quando a conexão **cai**, as leituras passam a ser armazenadas no buffer local.
  O Monitor Serial registra o acúmulo (`[EDGE] Offline. Leitura armazenada...`).
- Quando a conexão **volta**, a função `sincronizarBuffer()` esvazia o buffer na
  ordem de coleta (FIFO) e só remove cada leitura **após** a confirmação de envio.
  Isso é o equivalente lógico de "apagar o arquivo local depois de enviar",
  garantindo que nenhum dado se perca caso o envio falhe no meio do processo.

A simulação de conectividade é feita por uma variável booleana que alterna de
estado a cada 30 segundos, reproduzindo o ciclo online/offline de forma observável
durante a execução.

### Política quando o buffer enche: FIFO

Quando o buffer atinge sua capacidade máxima, a leitura **mais antiga** é descartada
para dar lugar à mais recente. Em monitoramento cardíaco, o dado mais recente tem
maior valor clínico: se o dispositivo ficou offline por muito tempo, é mais
importante preservar o estado atual do paciente do que leituras de minutos atrás.

---

## 4. Dimensionamento do buffer — alinhamento com o modelo de negócio

O enunciado pede explicitamente uma estratégia de armazenamento limitado alinhada
ao modelo de negócio, incluindo quantas amostras o dispositivo registra até voltar
a ficar online. O dimensionamento foi feito com base em números reais do ESP32:

| Item | Valor |
|---|---|
| Tamanho de cada amostra (struct `Leitura`) | ~48 bytes |
| Capacidade do buffer | 200 amostras |
| Memória total ocupada pelo buffer | ~9,6 KB |
| RAM livre típica no ESP32 | ~320 KB |
| Fração da RAM usada pelo buffer | ~3% |
| Intervalo entre amostras | 5 segundos |
| **Cobertura offline** | **200 × 5 s = 1000 s ≈ 16,6 minutos** |

**Decisão de projeto:** 200 amostras equivalem a cerca de 16,6 minutos de operação
offline contínua, usando apenas 3% da RAM disponível. Esse valor cobre a maioria
das quedas de conexão em deslocamentos urbanos sem comprometer a estabilidade do
dispositivo. Caso o modelo de negócio exija janelas offline maiores (por exemplo,
monitoramento em áreas rurais), duas opções escalam a solução sem reescrever a
lógica: aumentar a capacidade do buffer (há margem de RAM) ou reduzir a frequência
de amostragem em modo offline para economizar espaço.

---

## 5. Papel do Edge Computing em aplicações críticas de saúde

Processar e armazenar dados na borda, antes de enviar para a nuvem, traz
benefícios diretos para este caso de uso:

- **Resiliência:** o monitoramento continua mesmo sem rede — essencial em saúde.
- **Menor latência no alerta local:** o LED do dispositivo acende imediatamente
  quando um limite é ultrapassado, sem depender de ida e volta à nuvem.
- **Economia de banda e energia:** dados são agrupados e enviados quando há
  conexão, em vez de uma tentativa de transmissão a cada leitura.
- **Privacidade:** o processamento inicial ocorre no dispositivo, reduzindo a
  exposição de dados sensíveis de saúde em trânsito.

Essas características tornam o Edge Computing não um detalhe de implementação, mas
um requisito de segurança e confiabilidade em dispositivos médicos vestíveis.

---

## 6. Artefatos desta parte

| Artefato | Localização | Observação |
|---|---|---|
| Código do firmware | `src/pulso_band.ino` | C++ comentado |
| Circuito | `wokwi/diagram.json` | ESP32 + DHT22 + botão |
| Bibliotecas | `wokwi/libraries.txt` | DHT, PubSubClient, ArduinoJson |
| Link do projeto no Wokwi | README (seção Parte 1) | — |

> Nota de rastreabilidade: o buffer e a sincronização existem em tempo de execução
> (RAM do ESP32). Não há arquivo de dados persistido no repositório para esta parte,
> pois o SPIFFS é volátil no simulador — a evidência é o log do Monitor Serial.
