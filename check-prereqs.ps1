# check-prereqs.ps1
# Verifica todos os pre-requisitos para buildar o RCO (servidor + cliente)
# Uso: powershell -ExecutionPolicy Bypass -File check-prereqs.ps1

$ok   = 0
$warn = 0
$fail = 0

function OK($msg)   { Write-Host "  [OK]    $msg" -ForegroundColor Green;  $global:ok++   }
function WARN($msg) { Write-Host "  [AVISO] $msg" -ForegroundColor Yellow; $global:warn++ }
function FAIL($msg) { Write-Host "  [FALTA] $msg" -ForegroundColor Red;    $global:fail++ }

function Section($title) {
    Write-Host ""
    Write-Host "--- $title ---" -ForegroundColor Cyan
}

Write-Host ""
Write-Host "================================================" -ForegroundColor Cyan
Write-Host "  RealmCrafter: Origins - verificacao de deps  " -ForegroundColor Cyan
Write-Host "================================================" -ForegroundColor Cyan

# ---------------------------------------------------------------------------
Section "SERVIDOR (Go)"

$go = Get-Command go -ErrorAction SilentlyContinue
if ($go) {
    $goVer = (go version) -replace "go version go", "" -replace " windows.*", ""
    $parts = $goVer -split "\."
    $major = [int]$parts[0]
    $minor = [int]$parts[1]
    if ($major -ge 1 -and $minor -ge 22) {
        OK "Go $goVer"
    } else {
        WARN "Go $goVer instalado, mas precisa de 1.22+  ->  https://go.dev/dl/"
    }
} else {
    FAIL "Go nao encontrado  ->  https://go.dev/dl/  (ou: winget install GoLang.Go)"
}

if (Test-Path "$PSScriptRoot\server\go.mod") {
    OK "server/go.mod encontrado"
} else {
    FAIL "server/go.mod nao encontrado - repositorio incompleto"
}

# ---------------------------------------------------------------------------
Section "CLIENTE (C++)"

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) {
    $cmakeVerStr = (cmake --version | Select-Object -First 1) -replace "cmake version ", ""
    $parts = $cmakeVerStr -split "\."
    $major = [int]$parts[0]
    $minor = [int]$parts[1]
    if ($major -ge 3 -and $minor -ge 20) {
        OK "CMake $cmakeVerStr"
    } else {
        WARN "CMake $cmakeVerStr instalado, mas precisa de 3.20+  ->  https://cmake.org/download/"
    }
} else {
    FAIL "CMake nao encontrado  ->  winget install Kitware.CMake"
}

$cl = Get-Command cl -ErrorAction SilentlyContinue
if ($cl) {
    OK "MSVC (cl.exe): $($cl.Source)"
} else {
    WARN "cl.exe nao encontrado no PATH"
    Write-Host "         Abra o 'Developer PowerShell for VS 2022' e rode novamente" -ForegroundColor DarkGray
    Write-Host "         Ou instale: https://visualstudio.microsoft.com/" -ForegroundColor DarkGray
}

$git = Get-Command git -ErrorAction SilentlyContinue
if ($git) {
    OK "Git: $($git.Source)"
} else {
    WARN "Git nao encontrado  ->  https://git-scm.com/"
}

# ---------------------------------------------------------------------------
Section "VCPKG"

$vcpkgExe  = $null
$vcpkgPath = $null

$candidates = @(
    "C:\vcpkg\vcpkg.exe",
    "$env:USERPROFILE\vcpkg\vcpkg.exe",
    "C:\tools\vcpkg\vcpkg.exe"
)

$inPath = Get-Command vcpkg -ErrorAction SilentlyContinue
if ($inPath) {
    $vcpkgExe  = $inPath.Source
    $vcpkgPath = Split-Path $inPath.Source
} else {
    foreach ($c in $candidates) {
        if (Test-Path $c) {
            $vcpkgExe  = $c
            $vcpkgPath = Split-Path $c
            break
        }
    }
}

if ($vcpkgExe) {
    OK "vcpkg: $vcpkgPath"
} else {
    FAIL "vcpkg nao encontrado"
    Write-Host "         Para instalar:" -ForegroundColor DarkGray
    Write-Host "           git clone https://github.com/microsoft/vcpkg C:\vcpkg" -ForegroundColor DarkGray
    Write-Host "           C:\vcpkg\bootstrap-vcpkg.bat" -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
Section "PACOTES VCPKG (Phase 1)"

$requiredPkgs = @(
    [PSCustomObject]@{ name = "glfw3";  desc = "janela e input" },
    [PSCustomObject]@{ name = "glad";   desc = "OpenGL loader"  },
    [PSCustomObject]@{ name = "glm";    desc = "math vetores/matrizes" },
    [PSCustomObject]@{ name = "imgui";  desc = "UI Dear ImGui" },
    [PSCustomObject]@{ name = "msquic"; desc = "rede QUIC" }
)

if ($vcpkgExe) {
    $installed = (& $vcpkgExe list 2>$null) -join "`n"

    $missingNames = @()
    foreach ($pkg in $requiredPkgs) {
        if ($installed -match "$($pkg.name):x64-windows") {
            OK "$($pkg.name):x64-windows  ($($pkg.desc))"
        } else {
            FAIL "$($pkg.name):x64-windows  ($($pkg.desc))  ->  nao instalado"
            $missingNames += $pkg.name
        }
    }

    if ($installed -match "assimp:x64-windows") {
        OK "assimp:x64-windows  (Phase 2 - modelos 3D)"
    } else {
        Write-Host "  [INFO]  assimp:x64-windows  nao instalado (so necessario na Phase 2)" -ForegroundColor DarkGray
    }

    if ($missingNames.Count -gt 0) {
        Write-Host ""
        Write-Host "  Para instalar os pacotes em falta:" -ForegroundColor Yellow
        $pkgList = $missingNames -join " "
        Write-Host "  $vcpkgExe install $pkgList imgui[glfw-binding,opengl3-binding] --triplet x64-windows" -ForegroundColor Yellow
    }
} else {
    WARN "Pulando verificacao de pacotes - vcpkg nao encontrado"
}

# ---------------------------------------------------------------------------
Section "RESUMO"

$total = $ok + $warn + $fail
Write-Host ""
Write-Host "  OK:     $ok / $total" -ForegroundColor Green
if ($warn -gt 0) { Write-Host "  Avisos: $warn / $total" -ForegroundColor Yellow }
if ($fail -gt 0) { Write-Host "  Falta:  $fail / $total" -ForegroundColor Red }
Write-Host ""

if ($fail -eq 0 -and $warn -eq 0) {
    Write-Host "  Tudo pronto! Proximos passos:" -ForegroundColor Green
    Write-Host "    cd server" -ForegroundColor DarkGray
    Write-Host "    go mod tidy" -ForegroundColor DarkGray
    Write-Host "    go build -o server.exe ./cmd/server" -ForegroundColor DarkGray
    Write-Host "    ./server.exe" -ForegroundColor DarkGray
} elseif ($fail -eq 0) {
    Write-Host "  Quase la! Resolva os avisos acima e tente novamente." -ForegroundColor Yellow
} else {
    Write-Host "  Instale os itens em vermelho antes de buildar." -ForegroundColor Red
}
Write-Host ""
