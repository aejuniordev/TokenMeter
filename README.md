# TokenMeter para Windows

[![Build e Release](../../actions/workflows/build.yml/badge.svg)](../../actions/workflows/build.yml)

**Download:** baixe o `.zip` mais recente na página [Releases](../../releases/latest).

Janelinha nativa, sempre visível, que mostra:

- **quanto da janela de contexto já foi usada** (em %);
- **quanto tempo falta para seus limites resetarem**: as janelas de 5 horas e semanal da assinatura Pro/Max e os limites da API.

Outras características:

- Um `.exe` de ~250 KB em C puro + Win32. Não precisa instalar nada nem de runtime.
- Fica por cima das outras janelas sem roubar o foco do seu programa.
- Percentuais e barras que mudam de cor: verde → amarelo (60%) → vermelho (85%).
- Contagem regressiva ao vivo ("2h 14min") e o horário do reset ("15:35", "ter 16:34").
- Ícone na bandeja (perto do relógio) com um gráfico de pizza. Passe o mouse para ver os resets.
- **Ícone animado na barra de tarefas:** um anel com o percentual no meio, que enche e muda de cor conforme o uso, desliza suavemente quando o valor muda, gira enquanto consulta ou recebe streaming e "pulsa" acima de 85%. O botão na barra de tarefas também ganha a barra de progresso nativa do Windows (verde, amarelo ou vermelho).
- Dois botões no canto do cabeçalho: **↻ atualizar** os limites na hora e **🐞 debug** (log e erros). Um ponto vermelho no 🐞 avisa que houve erro.
- Arrastável. Duplo clique alterna para o **modo compacto** (`● 25,4%   5h: 2h 14min`).
- Lembra posição e preferências. Segue o tema claro/escuro do Windows. Funciona em telas com escala (DPI).

## Instalação

1. Baixe o `TokenMeter-<versão>-win64.zip` em [Releases](../../releases/latest) e extraia a pasta num lugar **sem espaços no caminho**, por exemplo `C:\Ferramentas\TokenMeter`.
2. Dê dois cliques em `TokenMeter.exe`. A janelinha aparece no canto inferior direito.
3. Opcional: clique com o botão direito nela → **Iniciar com o Windows**.

> **Aviso do SmartScreen:** o `.exe` não é assinado digitalmente, então o Windows pode mostrar "O Windows protegeu o computador". Clique em **Mais informações → Executar assim mesmo**. Se preferir, compile você mesmo a partir do código-fonte incluído (veja abaixo).

Para ver tudo funcionando sem API key nem assinatura, abra o PowerShell na pasta `exemplos` e rode:

```powershell
powershell -ExecutionPolicy Bypass -File testar.ps1
```

---

## Limites da assinatura Pro/Max (5 horas e semana)

Esses limites valem para o claude.ai e para o Claude Code juntos. Quem sabe o percentual usado e o horário de reset é o **Claude Code**, e o TokenMeter pergunta isso a ele sozinho.

**O que você precisa:** ter o [Claude Code](https://code.claude.com) instalado e ter feito login uma vez com sua conta Pro ou Max (rode `claude` no terminal e escolha a conta do claude.ai). Depois disso, não precisa abrir o Claude Code de novo.

**Como funciona:** ao abrir, e depois a cada 5 minutos, o TokenMeter chama o Claude Code escondido, em segundo plano, e pede os mesmos dados que o comando `/usage` mostra. Isso **não envia mensagem ao Claude e não gasta seu limite**; leva cerca de um segundo. As linhas **5 horas** e **Semana** aparecem com a contagem regressiva, e o cabeçalho mostra o plano ("Max · atualizado há 2 min").

- O uso que você faz no claude.ai entra na próxima consulta, em até 5 minutos.
- A contagem regressiva anda a cada segundo entre uma consulta e outra.
- Quando a janela de 5 horas vira, a linha mostra **liberado**.
- O botão **↻** no cabeçalho (ou botão direito → **Atualizar limites agora**) força uma consulta na hora. O ícone gira enquanto consulta.
- O menu mostra se deu certo: "atualizados às 09:41", "Claude Code não encontrado", "não está logado com Pro/Max" ou "atualize o Claude Code".

> Essa consulta usa um recurso do Claude Code que o próprio Agent SDK oficial expõe, mas que ainda é marcado como **experimental**: uma versão futura do Claude Code pode mudá-lo. Se isso acontecer, o menu vai avisar, e a alternativa abaixo continua funcionando.

Para mudar o intervalo, use `refresh_minutes` no `.ini` (`0` desliga). Se o TokenMeter não achar o Claude Code, informe o caminho em `claude_path`, por exemplo `C:\Users\voce\.local\bin\claude.exe`.

### Alternativa: status line do Claude Code

Também dá para o Claude Code mandar os limites a cada resposta, enquanto você o usa. Botão direito no TokenMeter → **Conectar ao Claude Code (status line)**. Isso grava a configuração em `%USERPROFILE%\.claude\settings.json` e salva uma cópia de segurança (`settings.json.antes-do-tokenmeter`). Se você já tiver outra status line, ele pergunta antes de substituir.

De quebra, a linha de status do Claude Code passa a mostrar:

```
Opus 5.5 · contexto 25% · 5h 64% (reseta em 2h 14min) · semana 41% (reseta em 3d 4h)
```

Para configurar à mão, use o menu **Copiar comando da status line** e cole no `settings.json`:

```json
{
  "statusLine": {
    "type": "command",
    "command": "C:/Ferramentas/TokenMeter/TokenMeter.exe --statusline"
  }
}
```

Use barras normais (`/`) no caminho: o Git Bash, que o Claude Code usa no Windows, apaga as barras invertidas.

---

## Limites da API (chave `sk-...`)

A API não tem um "pacote de tokens" que zera de tempos em tempos. Ela tem dois limites, e o TokenMeter mostra ambos:

| Linha | O que é | De onde vem |
|---|---|---|
| **API/min** | Tokens por minuto. O balde vai se enchendo de novo continuamente. Mostra quanto do balde está em uso e em quantos segundos ele estará cheio. Se você levar um erro 429, mostra **espere Xs**. | Cabeçalhos `anthropic-ratelimit-*` e `retry-after` de cada resposta |
| **Mês API** | O teto de gasto mensal do seu nível. Zera às 00:00 UTC do dia 1º (21:00 do último dia do mês, no horário de Brasília). Fica vermelho em **100%** se você atingir o teto. | Calendário, mais o erro `enforced_spend_limit_reached` |

O valor gasto no mês não vem nas respostas da API. Para ver quanto já gastou, use o painel do [Claude Console](https://platform.claude.com/usage).

Para alimentar essas linhas, o seu programa manda os cabeçalhos da resposta para `POST /limits` (veja os exemplos abaixo).

---

## Como o seu programa manda os dados

O TokenMeter escuta em `http://127.0.0.1:47821`, só na sua máquina: nada é exposto na rede e o Firewall não pergunta nada.

| Rota | Corpo |
|---|---|
| `POST /usage` | a resposta inteira da API (`Message`) ou só o objeto `usage` |
| `POST /event` | um evento de streaming (`message_start`, `message_delta`, `message_stop`; os outros são ignorados) |
| `POST /limits` | os cabeçalhos da resposta como objeto JSON, o corpo de um erro 429, ou o JSON da status line do Claude Code |
| `POST /config` | `{"model": "...", "context_window": 1000000}`, ou a resposta da Models API (lê `max_input_tokens`) |
| `POST /reset` | zera o contexto (nova conversa). `{"all": true}` zera também os totais |
| `GET /state` | estado atual em JSON, incluindo `limits` com `seconds_left` de cada janela |

### Python

Copie `exemplos/tokenmeter_client.py` para o seu projeto. Ele envia em segundo plano e nunca trava nem quebra o seu programa, mesmo com o TokenMeter fechado.

```python
from tokenmeter_client import report_event, report_headers, report_error

try:
    with client.messages.stream(model="claude-opus-5-5", max_tokens=8192, messages=messages) as stream:
        report_headers(stream.response.headers)   # limites da API
        for event in stream:
            report_event(event)                   # contexto ao vivo
            ...
except anthropic.RateLimitError as e:
    report_error(e)                               # "espere Xs" / teto mensal
```

Sem streaming:

```python
raw = client.messages.with_raw_response.create(model="claude-opus-5-5", max_tokens=8192, messages=messages)
report_headers(raw.headers)
msg = raw.parse()
report(msg)
```

Exemplo completo de chat no terminal: `exemplos/exemplo_python.py`.

### Node.js

```js
const { data: msg, response } = await client.messages.create({ ... }).withResponse();
meter('/limits', Object.fromEntries(response.headers));
meter('/usage', msg);
```

Com streaming e tratamento de 429: `exemplos/exemplo_node.mjs`.

### Qualquer outra linguagem (C#, Go, Rust, Java…)

Basta um POST HTTP com JSON:

```bash
curl -X POST http://127.0.0.1:47821/usage  -d "{\"usage\":{\"input_tokens\":5000,\"output_tokens\":800}}"
curl -X POST http://127.0.0.1:47821/limits -d "{\"anthropic-ratelimit-tokens-limit\":\"2000000\",\"anthropic-ratelimit-tokens-remaining\":\"1500000\",\"anthropic-ratelimit-tokens-reset\":\"2026-10-31T12:30:00Z\"}"
```

Use um timeout curto e ignore erros, para o medidor nunca atrapalhar o seu programa.

---

## Como o % de contexto é calculado

```
usado = input_tokens + cache_read_input_tokens + cache_creation_input_tokens + output_tokens
%     = usado / janela de contexto
```

A janela padrão é **1.000.000 tokens**, o tamanho dos modelos atuais (Fable 5.1, Opus 5.5, Sonnet 5.5, Haiku 5.5). Quando os dados vêm do Claude Code, o TokenMeter usa o tamanho que o Claude Code informa.

Se você usa ao mesmo tempo o seu programa e o Claude Code, o contexto mostrado é o de quem mandou dados por último. Os limites aparecem todos juntos.

## Debug: ver erros e mensagens internas

Clique no **🐞** no canto da janelinha (ou botão direito → **Abrir debug**). Abre uma janela com:

- **Resumo:** se o servidor local está rodando e em qual porta, onde estão as configurações e o log, qual Claude Code foi encontrado, o status da última consulta, os limites atuais e o contexto.
- **Log ao vivo:** cada consulta ao Claude Code (o comando usado, o tempo de resposta e o que voltou), as mensagens de erro que o próprio Claude Code imprimiu, cada dado que o seu programa enviou e os pedidos recusados.
- Botões: **Atualizar limites agora**, **Copiar tudo** (para colar numa conversa ou num relato de problema), **Limpar log**, **Abrir arquivo de log** e **Abrir configurações**.

O log também fica gravado em `%APPDATA%\TokenMeter.log`. Passando de 1 MB, o arquivo anterior vira `TokenMeter.log.old`.

Quando acontece um erro com a janela de debug fechada, aparece um **ponto vermelho** no 🐞. Se ainda não há limites para mostrar, a janelinha exibe o motivo numa linha vermelha, por exemplo "Limites: Claude Code não encontrado".

Mensagens comuns:

| No log | O que fazer |
|---|---|
| Claude Code não encontrado | Instale o Claude Code ou informe o caminho do `claude.exe` em `claude_path` no `.ini` |
| sem limites de assinatura (rate_limits_available=false) | O Claude Code está logado com API key ou sem login. Rode `claude`, use `/login` e escolha a conta Pro/Max |
| Claude Code recusou o pedido get_usage | Versão antiga. Rode `claude update` |
| fechou sem responder (código de saída …) | Veja a linha "Saída do Claude Code" logo acima. Em geral, pede login ou atualização |
| não respondeu em 60 s | Internet lenta ou Claude Code travado. Tente de novo no ↻ |
| recusado: veio de uma página web | Um `fetch` do navegador tentou enviar dados. Use `cors=1` no `.ini` se for seu |

## Ícone na barra de tarefas

O TokenMeter aparece na barra de tarefas com um ícone que mostra o percentual:

- **Anel colorido com o número no meio:** verde até 60%, amarelo até 85% e vermelho acima disso (os limites mudam em `warn` e `danger` no `.ini`).
- **Animação:** quando chega um valor novo, o anel desliza até ele. Enquanto o TokenMeter consulta o Claude Code ou recebe um streaming, um brilho gira em volta. Acima do limite vermelho, o anel "respira" para chamar a atenção.
- **Barra de progresso no próprio botão,** na mesma cor. Durante a consulta, vira a animação de "carregando" do Windows.
- **Título do botão** (aparece ao passar o mouse): `TokenMeter · contexto 25% · 5h 64% · semana 41%`.
- O ícone da bandeja, perto do relógio, acompanha a mesma animação, em tamanho menor.

Por padrão, o ícone mostra **o maior** entre contexto, limite de 5 horas e limite da semana, ou seja, o que estiver mais perto de acabar. Para trocar: botão direito → **Ícone da barra de tarefas** → Contexto, Limite de 5 horas, Limite da semana ou **Sem botão na barra de tarefas** (volta a ficar só perto do relógio).

Clicar no botão da barra de tarefas minimiza ou mostra a janelinha.

## Menu (botão direito na janela ou no ícone da bandeja)

Copiar endereço · Nova conversa · Zerar tudo · Modo compacto · Sempre visível · Janela de contexto (200k / 1M) · Ícone da barra de tarefas · Iniciar com o Windows · status dos limites · Atualizar limites agora · Abrir debug · Conectar ao Claude Code (status line) · Copiar comando da status line · Ocultar/Mostrar · Sair.

## Configurações

Ficam em `%APPDATA%\TokenMeter.ini`. Edite com o Bloco de Notas com o programa fechado:

```ini
[TokenMeter]
port=47821            ; porta local
context_window=1000000
warn=60               ; % para amarelo
danger=85             ; % para vermelho
theme=auto            ; auto | dark | light
topmost=1
compact=0
taskbar_icon=max      ; ícone da barra de tarefas: max | context | 5h | week | off
refresh_minutes=5     ; consulta os limites Pro/Max ao Claude Code (0 = desliga)
claude_path=          ; caminho do claude.exe, se não for achado sozinho
autolaunch=1          ; a status line do Claude Code abre o TokenMeter se estiver fechado
cors=0                ; 1 = aceita POST vindo de páginas web (fetch do navegador)
```

Opções de linha de comando: `--port 50000`, `--window 200000`, `--compact`, `--cors`, `--statusline`.

Por segurança, pedidos feitos por páginas web (com cabeçalho `Origin`) são recusados, a não ser que `cors=1`.

## Releases automáticas (GitHub Actions)

O workflow `.github/workflows/build.yml` roda a cada push e pull request:

1. Compila o `TokenMeter.exe` com MinGW-w64 no Ubuntu (`scripts/build.sh`).
2. Abre o programa no Wine e confere se o servidor, o contexto e os limites respondem (`scripts/smoke-test.sh`).
3. Monta o `.zip`, o `.exe` e o `SHA256SUMS.txt` (`scripts/package.sh`) e guarda tudo como artefato da execução, na aba **Actions**.
4. Num job separado e informativo, confere se o `build.bat` também compila com o Visual Studio.

Para **publicar uma versão**, crie e envie uma tag que comece com `v`:

```bash
git tag v1.3.0
git push origin v1.3.0
```

A pipeline grava a versão no `.exe` (Propriedades → Detalhes e janela de debug) e cria a Release `TokenMeter 1.3.0` com os três arquivos e as notas geradas a partir dos commits. Uma tag com hífen, como `v1.4.0-beta.1`, sai marcada como pré-release.

## Compilar você mesmo

O código está todo em `tokenmeter.c`.

- **Windows:** com **Visual Studio Build Tools** (no "Developer Command Prompt") ou **MinGW-w64** instalado, rode `build.bat`.
- **Linux, MSYS2 ou w64devkit:** `./scripts/build.sh 1.3.0` gera `dist/TokenMeter.exe`. No Linux, precisa do pacote `gcc-mingw-w64-x86-64`.

À mão, com MinGW:

```
windres tokenmeter.rc -O coff -o tokenmeter.res
gcc -O2 -s -municode -mwindows tokenmeter.c tokenmeter.res -o TokenMeter.exe -lws2_32 -lgdi32 -luser32 -lshell32 -ldwmapi -ladvapi32 -lcomctl32 -lole32 -luuid
```

## Arquivos

| Arquivo | O que é |
|---|---|
| `TokenMeter.exe` | o programa (vem nas [Releases](../../releases/latest); Windows 10/11, 64 bits) |
| `tokenmeter.c` | código-fonte |
| `tokenmeter.rc`, `.manifest`, `.ico` | ícone, versão e suporte a DPI |
| `build.bat` | compila no Windows com MSVC ou MinGW |
| `scripts/build.sh` | compila com MinGW (Linux/MSYS2), com a versão gravada no `.exe` |
| `scripts/smoke-test.sh` | testa o `.exe` no Wine |
| `scripts/package.sh` | monta o `.zip` e o `SHA256SUMS.txt` da release |
| `.github/workflows/build.yml` | pipeline de build e release |
| `exemplos/tokenmeter_client.py` | cliente Python para copiar no seu projeto |
| `exemplos/exemplo_python.py` | chat de terminal com streaming e limites |
| `exemplos/exemplo_node.mjs` | exemplo em Node.js |
| `exemplos/testar.ps1` | simulação sem API key |
