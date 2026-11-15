# Copia el port armado (C:\Users\Public\choya-port) a la tarjeta de la consola y COMPRUEBA que llego.
#
#   powershell -ExecutionPolicy Bypass -File scripts\copiar-a-consola.ps1            (busca la tarjeta sola)
#   powershell -ExecutionPolicy Bypass -File scripts\copiar-a-consola.ps1 -Destino K:\roms\ports
#
# Busca una unidad (que no sea C:) con la carpeta roms\ports, copia con robocopy reescribiendo todo y al final compara
# version.txt, el lanzador y el programa (WiiCompiled) de los dos lados. Si algo no coincide lo dice en rojo.
param([string]$Destino = "")

$origen = "C:\Users\Public\choya-port"
$verOrigen = Join-Path $origen "wiisports-choya\version.txt"
if (-not (Test-Path $verOrigen)) {
    Write-Host "No hay un port nuevo en $origen (falta wiisports-choya\version.txt): compila primero en Ubuntu." -ForegroundColor Red
    exit 1
}

if (-not $Destino) {
    $candidatas = @()
    foreach ($d in Get-PSDrive -PSProvider FileSystem) {
        if ($d.Root -like "C:*") { continue }
        $p = Join-Path $d.Root "roms\ports"
        if (Test-Path $p) { $candidatas += $p }
    }
    if ($candidatas.Count -eq 0) {
        Write-Host "No encontre ninguna unidad con roms\ports. Unidades: $((Get-PSDrive -PSProvider FileSystem).Root -join ', ')" -ForegroundColor Red
        Write-Host "Pon la tarjeta en la PC o pasa la carpeta: -Destino K:\roms\ports"
        exit 1
    }
    $conPort = $candidatas | Where-Object { Test-Path (Join-Path $_ "wiisports-choya") }
    $Destino = if ($conPort) { @($conPort)[0] } else { $candidatas[0] }
    if ($candidatas.Count -gt 1) { Write-Host "Varias unidades con roms\ports: $($candidatas -join ', '). Uso $Destino" -ForegroundColor Yellow }
}

Write-Host "Port nuevo:  $(Get-Content $verOrigen)"
$verDestino = Join-Path $Destino "wiisports-choya\version.txt"
if (Test-Path $verDestino) { Write-Host "En la tarjeta antes: $(Get-Content $verDestino)" } else { Write-Host "En la tarjeta antes: (sin version.txt: anterior a 0.31.2)" }
Write-Host "Copiando $origen -> $Destino ..."

# /IS /IT: reescribe aunque robocopy crea que los archivos son iguales.
robocopy $origen $Destino /E /IS /IT /COPY:DT /DCOPY:D /FFT /R:2 /W:5 /NP /NFL /NDL /NJH
$rc = $LASTEXITCODE
if ($rc -ge 8) {
    Write-Host "ERROR: robocopy no pudo copiar todo (codigo $rc). Si la consola esta encendida y conectada por red, cierra el juego y repite." -ForegroundColor Red
    exit 1
}

# Comprobar que llego lo mismo.
$bien = $true
foreach ($rel in @("wiisports-choya\version.txt", "wiisports-choya\compilado.txt", "Wii Sports (Choya).sh", "wiisports-choya\WiiCompiled")) {
    $a = Join-Path $origen $rel
    $b = Join-Path $Destino $rel
    if (-not (Test-Path $b)) { Write-Host "FALTA en la tarjeta: $rel" -ForegroundColor Red; $bien = $false; continue }
    $ha = (Get-FileHash $a -Algorithm SHA1).Hash
    $hb = (Get-FileHash $b -Algorithm SHA1).Hash
    if ($ha -ne $hb) { Write-Host "DISTINTO en la tarjeta: $rel" -ForegroundColor Red; $bien = $false }
}
if ($bien) {
    Write-Host "Listo: la tarjeta tiene $(Get-Content $verDestino)" -ForegroundColor Green
    Write-Host "En el resumen de la consola, la primera linea debe decir lo mismo (choya: lanzador ...; port ...)."
} else {
    Write-Host "La copia NO quedo igual. Revisa que la tarjeta no este protegida contra escritura y que sea la que usa la consola." -ForegroundColor Red
    exit 1
}
