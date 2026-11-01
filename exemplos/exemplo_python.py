"""
Chat de terminal com a Claude API mostrando contexto e limites no TokenMeter.

    pip install anthropic
    set ANTHROPIC_API_KEY=sk-...        (PowerShell: $env:ANTHROPIC_API_KEY="sk-...")
    python exemplo_python.py
"""
import anthropic
from tokenmeter_client import configure, report_event, report_headers, report_error

MODEL = "claude-opus-5-5"
client = anthropic.Anthropic()

# Tamanho real da janela de contexto do modelo (opcional: o padrão já é 1M)
try:
    info = client.models.retrieve(MODEL)
    configure(model=info.id, context_window=info.max_input_tokens)
except Exception:
    configure(model=MODEL)

messages = []
print("Digite sua mensagem (Ctrl+C para sair).")
while True:
    try:
        text = input("\nVocê: ").strip()
    except (KeyboardInterrupt, EOFError):
        break
    if not text:
        continue
    messages.append({"role": "user", "content": text})
    print("Claude: ", end="", flush=True)

    try:
        with client.messages.stream(model=MODEL, max_tokens=8192, messages=messages) as stream:
            report_headers(stream.response.headers)   # <- limites: tokens/min e quando enche de novo
            for event in stream:
                report_event(event)                   # <- contexto ao vivo
                if event.type == "content_block_delta" and event.delta.type == "text_delta":
                    print(event.delta.text, end="", flush=True)
            final = stream.get_final_message()
    except anthropic.RateLimitError as e:              # 429: "espere Xs" ou teto mensal atingido
        report_error(e)
        print(f"\n[limite atingido: {e.message}]")
        messages.pop()
        continue

    messages.append({"role": "assistant", "content": final.content})
    print()

# Sem streaming seria:
#   raw = client.messages.with_raw_response.create(model=MODEL, max_tokens=8192, messages=messages)
#   report_headers(raw.headers)
#   msg = raw.parse()
#   report(msg)
