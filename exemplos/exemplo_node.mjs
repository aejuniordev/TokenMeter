// Exemplo em Node.js 18+ (fetch nativo).
//   npm install @anthropic-ai/sdk
//   set ANTHROPIC_API_KEY=sk-...
//   node exemplo_node.mjs "Explique o que é uma janela de contexto"
import Anthropic from '@anthropic-ai/sdk';

const METER = 'http://127.0.0.1:47821';
const MODEL = 'claude-opus-5-5';

// Nunca deixa o seu programa quebrar se o TokenMeter estiver fechado
const meter = (path, body) =>
  fetch(METER + path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(1000)
  }).catch(() => {});

// Só os cabeçalhos que interessam: anthropic-ratelimit-* e retry-after
const limitHeaders = (headers) =>
  Object.fromEntries([...headers].filter(([k]) => k.startsWith('anthropic-') || k === 'retry-after'));

const client = new Anthropic();
const prompt = process.argv.slice(2).join(' ') || 'Olá!';

try {
  const stream = client.messages.stream({
    model: MODEL,
    max_tokens: 8192,
    messages: [{ role: 'user', content: prompt }]
  });

  // limites (tokens/min e quando o balde enche de novo)
  const { response } = await stream.withResponse();
  meter('/limits', limitHeaders(response.headers));

  // contexto ao vivo
  for await (const event of stream) {
    if (event.type.startsWith('message_')) await meter('/event', event);
    if (event.type === 'content_block_delta' && event.delta.type === 'text_delta') {
      process.stdout.write(event.delta.text);
    }
  }
  console.log();
} catch (err) {
  if (err instanceof Anthropic.RateLimitError) {
    // 429: mostra "espere Xs" ou o teto mensal atingido
    await meter('/limits', { ...limitHeaders(err.headers ?? new Headers()), error: err.error });
    console.error('Limite atingido:', err.message);
  } else {
    throw err;
  }
}

// Sem streaming:
//   const { data: msg, response } = await client.messages.create({ ... }).withResponse();
//   meter('/limits', limitHeaders(response.headers));
//   meter('/usage', msg);
