# Teste sem API key: simula uma conversa crescendo, um streaming e os limites com contagem regressiva.
# Abra o TokenMeter.exe e rode:  powershell -ExecutionPolicy Bypass -File testar.ps1
$u = "http://127.0.0.1:47821"

function Post($path, $obj) {
    $json = $obj | ConvertTo-Json -Depth 6 -Compress
    Invoke-RestMethod -Uri "$u$path" -Method Post -ContentType "application/json" -Body $json | Out-Null
}

try { Invoke-RestMethod "$u/state" | Out-Null }
catch { Write-Host "O TokenMeter não está aberto (ou usa outra porta)." -ForegroundColor Red; exit 1 }

Post "/config" @{ model = "claude-opus-5-5"; context_window = 200000 }
Post "/reset"  @{ all = $true }

$hist = 3000
foreach ($i in 1..9) {
    $saida = Get-Random -Minimum 2000 -Maximum 6000
    Post "/usage" @{ model = "claude-opus-5-5"; usage = @{
        input_tokens = 1500; cache_read_input_tokens = $hist; cache_creation_input_tokens = 2500; output_tokens = $saida } }
    $hist += 1500 + 2500 + $saida + 12000
    Write-Host ("Chamada {0}: ~{1:N0} tokens no contexto" -f $i, $hist)
    Start-Sleep -Milliseconds 700
}

Write-Host "Simulando streaming..."
Post "/event" @{ type = "message_start"; message = @{ model = "claude-opus-5-5"; usage = @{
    input_tokens = 1200; cache_read_input_tokens = $hist; cache_creation_input_tokens = 0; output_tokens = 1 } } }
foreach ($k in 1..25) {
    Post "/event" @{ type = "message_delta"; usage = @{ output_tokens = $k * 300 } }
    Start-Sleep -Milliseconds 150
}
Post "/event" @{ type = "message_stop" }

Write-Host "Simulando limites (Claude Code Pro/Max e API)..."
$agora = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
Post "/limits" @{ rate_limits = @{
    five_hour = @{ used_percentage = 63.5; resets_at = $agora + 2*3600 + 14*60 }
    seven_day = @{ used_percentage = 41.2; resets_at = $agora + 3*86400 + 4*3600 } } }
Post "/limits" @{
    "anthropic-ratelimit-tokens-limit"     = "2000000"
    "anthropic-ratelimit-tokens-remaining" = "1200000"
    "anthropic-ratelimit-tokens-reset"     = [DateTime]::UtcNow.AddSeconds(40).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'") }

Write-Host "Pronto. Estado final:"
Invoke-RestMethod "$u/state" | ConvertTo-Json -Depth 4
