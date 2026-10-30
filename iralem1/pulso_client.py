"""
Pulso Band - Ir Alem 1: Cliente REST + deteccao de risco + alerta por e-mail
=============================================================================
Projeto academico - Graduacao Tecnologica em IA - FIAP
Autor: Richard Schmitz | RM 567951

Fluxo implementado (conecta HTTP + bibliotecas Python + RPA aplicado a saude):

    1. Gera/define leituras de sinais vitais simuladas
    2. ENVIA cada leitura para a API REST (POST /api/sinais)
    3. RECEBE de volta a ultima leitura registrada (GET /api/sinais/ultima)
    4. Aplica a logica de deteccao de risco (taquicardia, febre, ausencia de
       movimento)
    5. Em caso de risco, dispara um e-mail de alerta automatizado (RPA)

A etapa de e-mail funciona em dois modos:
    - MODO_SIMULACAO = True  -> imprime o e-mail no console (padrao, nao exige
      credenciais nem conexao SMTP real). Suficiente para a entrega academica.
    - MODO_SIMULACAO = False -> envia de verdade via SMTP (preencher credenciais).

Como executar:
    1. Em um terminal:  python pulso_api.py
    2. Em outro terminal: python pulso_client.py
"""

import smtplib
import time
from email.mime.text import MIMEText

import requests

# ---------------------------------------------------------------------------
# CONFIGURACAO
# ---------------------------------------------------------------------------
API_BASE = "http://localhost:5001"

# Limites clinicos de risco (coerentes com o firmware do ESP32).
LIMITE_BPM_ALTO = 100      # taquicardia em repouso (> 100 bpm)
LIMITE_BPM_BAIXO = 40      # bradicardia severa (< 40 bpm)
LIMITE_TEMP = 37.8         # febre (> 37,8 C)

# Configuracao de e-mail.
MODO_SIMULACAO = True      # True = imprime no console; False = envia via SMTP
SMTP_HOST = "smtp.gmail.com"
SMTP_PORT = 587
EMAIL_REMETENTE = "pulso.band.alertas@example.com"
EMAIL_SENHA = "SUA_SENHA_DE_APP"       # senha de app, nunca a senha real
EMAIL_DESTINATARIO = "equipe.cardio@example.com"


# ---------------------------------------------------------------------------
# 1. LOGICA DE DETECCAO DE RISCO
# ---------------------------------------------------------------------------
def avaliar_risco(leitura):
    """
    Recebe uma leitura e retorna uma lista de condicoes de risco detectadas.
    Lista vazia = paciente estavel.
    """
    riscos = []

    bpm = leitura.get("bpm")
    temp = leitura.get("temperatura")
    movimento = leitura.get("movimento")

    if bpm is not None:
        if bpm > LIMITE_BPM_ALTO:
            riscos.append(f"Taquicardia: {bpm} bpm (limite {LIMITE_BPM_ALTO})")
        elif bpm < LIMITE_BPM_BAIXO:
            riscos.append(f"Bradicardia: {bpm} bpm (limite {LIMITE_BPM_BAIXO})")

    if temp is not None and temp > LIMITE_TEMP:
        riscos.append(f"Febre: {temp} C (limite {LIMITE_TEMP} C)")

    # Ausencia de movimento pode indicar queda/desmaio em um wearable.
    if movimento is not None and movimento is False:
        riscos.append("Ausencia de movimento detectada (possivel queda/desmaio)")

    return riscos


# ---------------------------------------------------------------------------
# 2. ENVIO DE E-MAIL AUTOMATIZADO (RPA)
# ---------------------------------------------------------------------------
def enviar_email_alerta(leitura, riscos):
    """
    Monta e dispara o e-mail de alerta. Em MODO_SIMULACAO, apenas imprime.
    """
    assunto = "[Pulso Band] ALERTA - Sinais vitais fora dos limites"
    corpo = (
        "Alerta automatico do sistema Pulso Band.\n\n"
        f"Leitura ID: {leitura.get('id')}\n"
        f"Recebido em: {leitura.get('recebido_em')}\n"
        f"BPM: {leitura.get('bpm')}\n"
        f"Temperatura: {leitura.get('temperatura')} C\n"
        f"Umidade: {leitura.get('umidade')} %\n\n"
        "Condicoes de risco detectadas:\n"
        + "\n".join(f"  - {r}" for r in riscos)
        + "\n\nAcao recomendada: verificar o paciente e acionar protocolo clinico.\n"
        "Mensagem academica - nao representa monitoramento clinico real.\n"
    )

    if MODO_SIMULACAO:
        print("\n" + "=" * 60)
        print("E-MAIL DE ALERTA (modo simulacao)")
        print("=" * 60)
        print(f"De:      {EMAIL_REMETENTE}")
        print(f"Para:    {EMAIL_DESTINATARIO}")
        print(f"Assunto: {assunto}")
        print("-" * 60)
        print(corpo)
        print("=" * 60 + "\n")
        return True

    # Envio real via SMTP (TLS).
    try:
        msg = MIMEText(corpo)
        msg["Subject"] = assunto
        msg["From"] = EMAIL_REMETENTE
        msg["To"] = EMAIL_DESTINATARIO

        with smtplib.SMTP(SMTP_HOST, SMTP_PORT) as servidor:
            servidor.starttls()
            servidor.login(EMAIL_REMETENTE, EMAIL_SENHA)
            servidor.send_message(msg)
        print("[EMAIL] Alerta enviado com sucesso via SMTP.")
        return True
    except Exception as e:
        print(f"[EMAIL] Falha ao enviar: {e}")
        return False


# ---------------------------------------------------------------------------
# 3. CLIENTE REST - ENVIO E RECEBIMENTO
# ---------------------------------------------------------------------------
def enviar_leitura(leitura):
    """POST de uma leitura para a API. Retorna a resposta JSON ou None."""
    try:
        resp = requests.post(f"{API_BASE}/api/sinais", json=leitura, timeout=5)
        if resp.status_code == 201:
            return resp.json()["leitura"]
        print(f"[REST] Erro no POST: {resp.status_code} - {resp.text}")
    except requests.RequestException as e:
        print(f"[REST] Falha de conexao no POST: {e}")
    return None


def obter_ultima_leitura():
    """GET da ultima leitura registrada na API. Retorna JSON ou None."""
    try:
        resp = requests.get(f"{API_BASE}/api/sinais/ultima", timeout=5)
        if resp.status_code == 200:
            return resp.json()
        print(f"[REST] Erro no GET: {resp.status_code}")
    except requests.RequestException as e:
        print(f"[REST] Falha de conexao no GET: {e}")
    return None


# ---------------------------------------------------------------------------
# 4. ORQUESTRACAO DO FLUXO
# ---------------------------------------------------------------------------
def processar(leitura_simulada):
    """Executa o ciclo completo para uma leitura: envio, recebimento, risco, alerta."""
    print(f"\n>>> Enviando leitura: {leitura_simulada}")

    registrada = enviar_leitura(leitura_simulada)
    if not registrada:
        return

    recebida = obter_ultima_leitura()
    if not recebida:
        return

    print(f"[REST] Leitura confirmada pela API (ID {recebida.get('id')}).")

    riscos = avaliar_risco(recebida)
    if riscos:
        print(f"[RISCO] {len(riscos)} condicao(oes) detectada(s):")
        for r in riscos:
            print(f"        - {r}")
        enviar_email_alerta(recebida, riscos)
    else:
        print("[RISCO] Paciente estavel. Nenhum alerta necessario.")


def main():
    # Verifica se a API esta no ar antes de comecar.
    try:
        requests.get(f"{API_BASE}/health", timeout=5)
    except requests.RequestException:
        print("A API nao esta respondendo. Rode 'python pulso_api.py' primeiro.")
        return

    # Conjunto de leituras simuladas: estaveis e de risco, incluindo um caso
    # feminino atipico (bpm normal, mas sem movimento = possivel desmaio),
    # mantendo a continuidade do eixo tematico do projeto.
    leituras = [
        {"bpm": 78, "temperatura": 36.5, "umidade": 50, "movimento": True},   # estavel
        {"bpm": 132, "temperatura": 37.1, "umidade": 48, "movimento": True},  # taquicardia
        {"bpm": 88, "temperatura": 38.4, "umidade": 55, "movimento": True},   # febre
        {"bpm": 90, "temperatura": 36.8, "umidade": 52, "movimento": False},  # sem movimento
        {"bpm": 36, "temperatura": 36.2, "umidade": 49, "movimento": True},   # bradicardia
    ]

    for leitura in leituras:
        processar(leitura)
        time.sleep(1)  # espaca as chamadas para facilitar a leitura do log


if __name__ == "__main__":
    main()
