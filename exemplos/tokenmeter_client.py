"""
Cliente mínimo do TokenMeter para Python (só biblioteca padrão).

    from tokenmeter_client import report, report_event, configure

    msg = client.messages.create(...)
    report(msg)                      # resposta normal

    with client.messages.stream(...) as stream:
        for event in stream:
            report_event(event)      # streaming: atualiza ao vivo

Se o TokenMeter estiver fechado, as chamadas falham em silêncio:
o seu programa nunca quebra por causa do medidor.
"""
import json
import queue
import threading
import time
import urllib.request

URL = "http://127.0.0.1:47821"
_EVENTOS = {"message_start", "message_delta", "message_stop"}


def _to_json(obj):
    if isinstance(obj, (str, bytes)):
        return obj if isinstance(obj, bytes) else obj.encode("utf-8")
    if hasattr(obj, "model_dump_json"):          # objetos do SDK anthropic (Pydantic)
        return obj.model_dump_json().encode("utf-8")
    return json.dumps(obj).encode("utf-8")      # dict comum


_queue = queue.Queue(maxsize=1000)
_down_until = 0.0  # medidor fechado: para de tentar por alguns segundos


def _send(path, body):
    global _down_until
    if time.monotonic() < _down_until:
        return
    try:
        req = urllib.request.Request(
            URL + path, data=body, method="POST",
            headers={"Content-Type": "application/json"})
        urllib.request.urlopen(req, timeout=1).read()
    except Exception:
        _down_until = time.monotonic() + 5  # medidor fechado: ignora


def _worker():
    while True:
        path, body = _queue.get()
        _send(path, body)
        _queue.task_done()


threading.Thread(target=_worker, daemon=True, name="tokenmeter").start()


def _post(path, obj, wait=False):
    """Envia em segundo plano, na ordem, sem nunca travar o seu programa."""
    try:
        body = _to_json(obj)
    except Exception:
        return
    if wait:
        _queue.join()
        _send(path, body)
        return
    try:
        _queue.put_nowait((path, body))
    except queue.Full:
        pass


def flush(timeout=2.0):
    """Espera os envios pendentes (útil no fim de um script curto)."""
    end = time.monotonic() + timeout
    while _queue.unfinished_tasks and time.monotonic() < end:
        time.sleep(0.01)


def report(message, wait=False):
    """Envia uma Message completa (ou só o objeto usage)."""
    _post("/usage", message, wait)


def report_event(event):
    """Envia um evento de streaming. Ignora sozinho os eventos que não interessam."""
    t = getattr(event, "type", None) or (event.get("type") if isinstance(event, dict) else None)
    if t in _EVENTOS:
        _post("/event", event)


def report_headers(headers):
    """Envia os cabeçalhos de rate limit da resposta (anthropic-ratelimit-*, retry-after).

    Sem streaming:  raw = client.messages.with_raw_response.create(...); report_headers(raw.headers)
    Com streaming:  with client.messages.stream(...) as stream: report_headers(stream.response.headers)
    """
    try:
        data = {k.lower(): v for k, v in dict(headers).items()
                if k.lower().startswith("anthropic-") or k.lower() == "retry-after"}
    except Exception:
        return
    if data:
        _post("/limits", data)


def report_error(err):
    """Envia um erro da API (ex.: anthropic.RateLimitError) para mostrar 'espere Xs' ou o teto mensal."""
    data = {}
    resp = getattr(err, "response", None)
    if resp is not None:
        data.update({k.lower(): v for k, v in dict(resp.headers).items()
                     if k.lower().startswith("anthropic-") or k.lower() == "retry-after"})
    body = getattr(err, "body", None)
    if body is not None:
        data["error"] = body
    if data:
        _post("/limits", data)


def configure(model=None, context_window=None):
    """Define o modelo exibido e/ou o tamanho da janela de contexto."""
    body = {}
    if model:
        body["model"] = model
    if context_window:
        body["context_window"] = int(context_window)
    _post("/config", body)


def reset(all=False):
    """Nova conversa. all=True também zera os totais da sessão."""
    _post("/reset", {"all": bool(all)})
