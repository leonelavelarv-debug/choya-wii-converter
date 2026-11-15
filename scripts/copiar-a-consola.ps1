# Copia el port armado (C:\Users\Public\choya-port) a la tarjeta de la consola y COMPRUEBA cada archivo.
#
#   powershell -ExecutionPolicy Bypass -File scripts\copiar-a-consola.ps1            (busca la tarjeta sola)
#   powershell -ExecutionPolicy Bypass -File scripts\copiar-a-consola.ps1 -Destino K:\roms\ports
#   powershell -ExecutionPolicy Bypass -File scripts\copiar-a-consola.ps1 -SoloVerificar   (no copia, solo compara)
#
# 0.32.0: el port trae wiisports-choya\manifiesto.json con el SHA-256 de cada archivo. Antes de copiar se comprueba
# que el port de la PC este completo; despues de copiar se comprueba CADA archivo del manifiesto en la tarjeta. Si
# falta uno o es distinto lo dice en rojo y termina con error. Nunca borra nada en la tarjeta: las partidas, los
# ajustes, los perfiles PGO, los archivos de opciones (con-reverb, etc.) y el disco (game\DATA) quedan como estaban.
param([string]$Destino = "", [string]$Origen = "C:\Users\Public\choya-port", [switch]$SoloVerificar)

$sep = [IO.Path]::DirectorySeparatorChar

function Leer-Manifiesto([string]$raiz) {
    $p = Join-Path (Join-Path $raiz "wiisports-choya") "manifiesto.json"
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return $null }
    try { return (Get-Content -LiteralPath $p -Raw -Encoding UTF8 | ConvertFrom-Json) } catch { return $null }
}

function Leer-Version([string]$raiz) {
    $p = Join-Path (Join-Path $raiz "wiisports-choya") "version.txt"
    if (Test-Path -LiteralPath $p -PathType Leaf) { return (Get-Content -LiteralPath $p -Encoding UTF8 | Select-Object -First 1) }
    return $null
}

# Problemas de una carpeta frente a un manifiesto: archivos que faltan o que no tienen el mismo SHA-256.
function Verificar([string]$raiz, $m) {
    $problemas = @()
    foreach ($e in $m.archivos) {
        $ruta = Join-Path $raiz ($e.ruta -replace '/', $sep)
        if (-not (Test-Path -LiteralPath $ruta -PathType Leaf)) { $problemas += "FALTA     $($e.ruta)"; continue }
        $largo = (Get-Item -LiteralPath $ruta).Length
        if ($largo -ne [int64]$e.bytes) { $problemas += "DISTINTO  $($e.ruta) (mide $largo bytes, deberia medir $($e.bytes))"; continue }
        $h = (Get-FileHash -LiteralPath $ruta -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($h -ne $e.sha256) { $problemas += "DISTINTO  $($e.ruta) (otro SHA-256)" }
    }
    return ,$problemas
}

$mOrigen = Leer-Manifiesto $Origen
if ($null -eq $mOrigen) {
    Write-Host "No hay un port verificable en $Origen (falta wiisports-choya\manifiesto.json): compila primero en Ubuntu con Choya 0.32.0 o mas nuevo." -ForegroundColor Red
    exit 1
}
$malOrigen = Verificar $Origen $mOrigen
if ($malOrigen.Count -gt 0) {
    Write-Host "El port de la PC ($Origen) no coincide con su propio manifiesto: no se copia." -ForegroundColor Red
    $malOrigen | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Write-Host "Vuelve a compilar (bash scripts/build-wiisports-arm64.sh)." -ForegroundColor Red
    exit 1
}

if (-not $Destino) {
    $candidatas = @()
    foreach ($d in Get-PSDrive -PSProvider FileSystem) {
        if ($d.Root -like "C:*") { continue }
        $p = Join-Path $d.Root "roms\ports"
        if (Test-Path -LiteralPath $p) { $candidatas += $p }
    }
    if ($candidatas.Count -eq 0) {
        Write-Host "No encontre ninguna unidad con roms\ports. Unidades: $((Get-PSDrive -PSProvider FileSystem).Root -join ', ')" -ForegroundColor Red
        Write-Host "Pon la tarjeta en la PC o pasa la carpeta: -Destino K:\roms\ports"
        exit 1
    }
    $conPort = $candidatas | Where-Object { Test-Path -LiteralPath (Join-Path $_ "wiisports-choya") }
    $Destino = if ($conPort) { @($conPort)[0] } else { $candidatas[0] }
    if ($candidatas.Count -gt 1) { Write-Host "Varias unidades con roms\ports: $($candidatas -join ', '). Uso $Destino" -ForegroundColor Yellow }
}

Write-Host "Port de la PC:       $(Leer-Version $Origen)"
$antes = Leer-Version $Destino
if ($antes) { Write-Host "En la tarjeta antes: $antes" } else { Write-Host "En la tarjeta antes: (sin version.txt)" }

if (-not $SoloVerificar) {
    Write-Host "Copiando $Origen -> $Destino ($($mOrigen.archivos.Count) archivos; no se borra nada en la tarjeta) ..."
    if (Get-Command robocopy -ErrorAction SilentlyContinue) {
        # /E sin /PURGE ni /MIR: solo agrega y reescribe. /IS /IT: reescribe aunque robocopy crea que son iguales.
        robocopy $Origen $Destino /E /IS /IT /COPY:DT /DCOPY:D /FFT /R:2 /W:5 /NP /NFL /NDL /NJH
        $rc = $LASTEXITCODE
        if ($rc -ge 8) {
            Write-Host "ERROR: robocopy no pudo copiar todo (codigo $rc). Si la consola esta encendida y conectada por red, cierra el juego y repite." -ForegroundColor Red
            exit 1
        }
    } else {
        New-Item -ItemType Directory -Force -Path $Destino | Out-Null
        Copy-Item -Path (Join-Path $Origen "*") -Destination $Destino -Recurse -Force
    }
}

# Comprobar cada archivo del manifiesto de la PC en la tarjeta (y que la tarjeta tenga ese mismo manifiesto).
$mal = Verificar $Destino $mOrigen
$mDestino = Leer-Manifiesto $Destino
if ($null -eq $mDestino) { $mal += "FALTA     wiisports-choya/manifiesto.json" }
elseif ($mDestino.identidad -ne $mOrigen.identidad) { $mal += "DISTINTO  wiisports-choya/manifiesto.json (la tarjeta tiene el port $($mDestino.identidad.Substring(0,12)))" }

if ($mal.Count -eq 0) {
    Write-Host "Listo: los $($mOrigen.archivos.Count) archivos del port estan en la tarjeta y son iguales (port $($mOrigen.identidad.Substring(0,12)))." -ForegroundColor Green
    Write-Host "La tarjeta tiene: $(Leer-Version $Destino)"
    Write-Host "En el resumen de la consola, la primera linea debe decir lo mismo (choya: lanzador ...; port ...)."
    exit 0
}
Write-Host "La copia NO quedo igual ($($mal.Count) problemas):" -ForegroundColor Red
$mal | Select-Object -First 30 | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
Write-Host "Revisa que la tarjeta no este protegida contra escritura, que tenga espacio y que sea la que usa la consola." -ForegroundColor Red
exit 1
