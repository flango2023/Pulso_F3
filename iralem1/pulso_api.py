"""
Pulso Band - Ir Alem 1: API REST de sinais vitais
===================================================
Projeto academico - Graduacao Tecnologica em IA - FIAP
Autor: Richard Schmitz | RM 567951

Servidor REST minimo (Flask) que simula o endpoint de nuvem do Pulso Band.
Recebe leituras de sinais vitais via POST e as disponibiliza via GET.

Endpoints:
    POST /api/sinais   -> recebe uma leitura (JSON) e armazena em memoria
    GET  /api/sinais   -> retorna todas as leituras armazenadas
    GET  /api/sinais/ultima -> retorna apenas a leitura mais recente
    GET  /health       -> verificacao de saude do servico

Como executar:
    pip install -r requirements.txt
    python pulso_api.py
    # O servidor sobe em http://localhost:5001
"""

from flask import Flask, request, jsonify
from datetime import datetime

app = Flask(__name__)

# Armazenamento em memoria (suficiente para a simulacao academica).
# Em producao, seria um banco de dados ou um time-series store.
leituras = []


@app.route("/health", methods=["GET"])
def health():
    """Verificacao simples de que o servico esta no ar."""
    return jsonify({"status": "ok", "total_leituras": len(leituras)}), 200


@app.route("/api/sinais", methods=["POST"])
def receber_sinais():
    """
    Recebe uma leitura de sinais vitais em JSON.
    Campos esperados: bpm, temperatura, umidade, movimento (opcional).
    """
    dado = request.get_json(silent=True)
    if not dado:
        return jsonify({"erro": "Corpo JSON ausente ou invalido"}), 400

    # Validacao minima dos campos obrigatorios.
    for campo in ("bpm", "temperatura"):
        if campo not in dado:
            return jsonify({"erro": f"Campo obrigatorio ausente: {campo}"}), 400

    leitura = {
        "id": len(leituras) + 1,
        "recebido_em": datetime.utcnow().isoformat() + "Z",
        "bpm": dado["bpm"],
        "temperatura": dado["temperatura"],
        "umidade": dado.get("umidade"),
        "movimento": dado.get("movimento"),
    }
    leituras.append(leitura)
    return jsonify({"mensagem": "Leitura registrada", "leitura": leitura}), 201


@app.route("/api/sinais", methods=["GET"])
def listar_sinais():
    """Retorna todas as leituras armazenadas."""
    return jsonify({"total": len(leituras), "leituras": leituras}), 200


@app.route("/api/sinais/ultima", methods=["GET"])
def ultima_leitura():
    """Retorna a leitura mais recente, ou 404 se nao houver nenhuma."""
    if not leituras:
        return jsonify({"erro": "Nenhuma leitura disponivel"}), 404
    return jsonify(leituras[-1]), 200


if __name__ == "__main__":
    # debug=True facilita o desenvolvimento; desligar em producao.
    # Porta 5001 para evitar conflito com o AirPlay Receiver do macOS (porta 5000).
    app.run(host="0.0.0.0", port=5001, debug=True)
