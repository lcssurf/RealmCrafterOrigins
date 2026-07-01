# check-prereqs.ps1
# Verifica todos os pre-requisitos para buildar o RCO (servidor + cliente)
# Uso: powershell -ExecutionPolicy Bypass -File check-prereqs.ps1
#      powershell -ExecutionPolicy Bypass -File check-prereqs.ps1 -Install   (instala o que faltar)

param(
    [switch]$Install
)

$ok   = 0
$warn = 0
$fail = 0

function OK($msg)   { Write-Host "  [OK]    $msg" -ForegroundColor Green;  $global:ok++   }
function WARN($msg) { Write-Host "  [AVISO] $msg" -ForegroundColor Yellow; $global:warn++ }
function FAIL($msg) { Write-Host "  [FALTA] $msg" -ForegroundColor Red;    $global:fail++ }

function HasMSVCTools() {
    if (Get-Command cl -ErrorAction SilentlyContinue) {
        return $true
    }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vcPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        return -not [string]::IsNullOrWhiteSpace($vcPath)
    }
    return $false
}

function GetLatestVSInstallVersion() {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        return & $vswhere -latest -products * -property installationVersion 2>$null
    }
    return ""
}

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
    if ($major -gt 1 -or ($major -eq 1 -and $minor -ge 22)) {
        OK "Go $goVer"
    } else {
        WARN "Go $goVer instalado, mas precisa de 1.22+  ->  https://go.dev/dl/"
    }
} else {
    if ($Install) {
        Write-Host "  [INSTALL] Go ausente - instalando via winget..." -ForegroundColor Magenta
        winget install -e --id GoLang.Go --accept-source-agreements --accept-package-agreements
        WARN "Go instalado - abra um novo terminal para atualizar o PATH e rode novamente"
    } else {
        FAIL "Go nao encontrado  ->  https://go.dev/dl/  (ou: winget install GoLang.Go)"
    }
}

if (Test-Path "$PSScriptRoot\server\go.mod") {
    OK "server/go.mod encontrado"
} else {
    FAIL "server/go.mod nao encontrado - repositorio incompleto"
}

# ---------------------------------------------------------------------------
Section "CLIENTE (C++)"

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
$cmakeMajor = 0
$cmakeMinor = 0
if ($cmake) {
    $cmakeVerStr = (cmake --version | Select-Object -First 1) -replace "cmake version ", ""
    $parts = $cmakeVerStr -split "\."
    $cmakeMajor = [int]$parts[0]
    $cmakeMinor = [int]$parts[1]
    if ($cmakeMajor -gt 3 -or ($cmakeMajor -eq 3 -and $cmakeMinor -ge 20)) {
        OK "CMake $cmakeVerStr"
    } else {
        WARN "CMake $cmakeVerStr instalado, mas precisa de 3.20+  ->  https://cmake.org/download/"
    }
} else {
    if ($Install) {
        Write-Host "  [INSTALL] CMake ausente - instalando via winget..." -ForegroundColor Magenta
        winget install -e --id Kitware.CMake --accept-source-agreements --accept-package-agreements
        WARN "CMake instalado - abra um novo terminal para atualizar o PATH e rode novamente"
    } else {
        FAIL "CMake nao encontrado  ->  winget install Kitware.CMake"
    }
}

$cl = Get-Command cl -ErrorAction SilentlyContinue
if (HasMSVCTools) {
    if ($cl) {
        OK "MSVC cl.exe in PATH: $($cl.Source)"
    } else {
        OK "MSVC C++ Build Tools detected via Visual Studio Installer"
        Write-Host "         cl.exe is not in this terminal PATH, but Visual Studio generator builds can still work." -ForegroundColor DarkGray
    }
} else {
    if ($Install) {
        Write-Host "  [INSTALL] MSVC ausente - instalando Visual Studio 2022 Build Tools..." -ForegroundColor Magenta
        winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override "--passive --norestart --wait --add Microsoft.VisualStudio.Workload.VCTools" --accept-source-agreements --accept-package-agreements
        if (HasMSVCTools) {
            $cl = Get-Command cl -ErrorAction SilentlyContinue
            if ($cl) {
                OK "MSVC (cl.exe): $($cl.Source)"
            } else {
                WARN "Build Tools instalado, mas cl.exe ainda nao entrou no PATH."
                Write-Host "         Rode em 'Developer PowerShell for Visual Studio' ou defina VCTools via VS Installer" -ForegroundColor DarkGray
            }
        } else {
            FAIL "Falha ao instalar Visual Studio Build Tools via winget"
        }
    } else {
        WARN "cl.exe nao encontrado no PATH"
        Write-Host "         Abra o 'Developer PowerShell for Visual Studio' e rode novamente" -ForegroundColor DarkGray
        Write-Host "         Ou instale: https://visualstudio.microsoft.com/" -ForegroundColor DarkGray
    }
}

$vsVersion = GetLatestVSInstallVersion
if ($vsVersion -like "18.*" -and $cmake) {
    if ($cmakeMajor -lt 4 -or ($cmakeMajor -eq 4 -and $cmakeMinor -lt 2)) {
        if ($Install) {
            Write-Host "  [INSTALL] Visual Studio 2026 detected - upgrading CMake for the VS 2026 generator..." -ForegroundColor Magenta
            winget upgrade -e --id Kitware.CMake --accept-source-agreements --accept-package-agreements
            if ($LASTEXITCODE -ne 0) {
                winget install -e --id Kitware.CMake --accept-source-agreements --accept-package-agreements
            }
            WARN "CMake upgrade attempted - open a new terminal and run check-prereqs.bat again"
        } else {
            WARN "Visual Studio 2026 detectado; o gerador 'Visual Studio 18 2026' precisa de CMake 4.2+"
            Write-Host "         Atualize o CMake ou instale Visual Studio 2022 Build Tools para usar o gerador VS 2022." -ForegroundColor DarkGray
        }
    }
}

$git = Get-Command git -ErrorAction SilentlyContinue
if ($git) {
    OK "Git: $($git.Source)"
} else {
    if ($Install) {
        Write-Host "  [INSTALL] Git ausente - instalando via winget..." -ForegroundColor Magenta
        winget install -e --id Git.Git --accept-source-agreements --accept-package-agreements
        WARN "Git instalado - abra um novo terminal para atualizar o PATH e rode novamente"
    } else {
        WARN "Git nao encontrado  ->  https://git-scm.com/"
    }
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
} elseif ($Install) {
    Write-Host "  [INSTALL] vcpkg ausente - clonando em C:\vcpkg..." -ForegroundColor Magenta
    git clone https://github.com/microsoft/vcpkg C:\vcpkg
    & C:\vcpkg\bootstrap-vcpkg.bat
    if (Test-Path "C:\vcpkg\vcpkg.exe") {
        $vcpkgExe  = "C:\vcpkg\vcpkg.exe"
        $vcpkgPath = "C:\vcpkg"
        OK "vcpkg instalado em $vcpkgPath"
    } else {
        FAIL "Falha ao instalar vcpkg - veja o log acima"
    }
} else {
    FAIL "vcpkg nao encontrado"
    Write-Host "         Para instalar:" -ForegroundColor DarkGray
    Write-Host "           git clone https://github.com/microsoft/vcpkg C:\vcpkg" -ForegroundColor DarkGray
    Write-Host "           C:\vcpkg\bootstrap-vcpkg.bat" -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
Section "PACOTES VCPKG (client/GUE)"

$requiredPkgs = @(
    [PSCustomObject]@{ name = "glfw3";  desc = "janela e input" },
    [PSCustomObject]@{ name = "glad";   desc = "OpenGL loader"  },
    [PSCustomObject]@{ name = "glm";    desc = "math vetores/matrizes" },
    [PSCustomObject]@{ name = "imgui";  desc = "UI Dear ImGui" },
    [PSCustomObject]@{ name = "assimp"; desc = "modelos 3D" },
    [PSCustomObject]@{ name = "stb";    desc = "image loading headers" },
    [PSCustomObject]@{ name = "msquic"; desc = "rede QUIC" },
    [PSCustomObject]@{ name = "miniaudio"; desc = "audio header" },
    [PSCustomObject]@{ name = "sqlite3"; desc = "GUE SQLite / unofficial-sqlite3" },
    [PSCustomObject]@{ name = "nlohmann-json"; desc = "GUE JSON validation" }
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

    if ($missingNames.Count -gt 0) {
        if ($Install) {
            $installNames = @()
            foreach ($name in $missingNames) {
                if ($name -eq "imgui") {
                    $installNames += "imgui[glfw-binding,opengl3-binding]"
                } else {
                    $installNames += $name
                }
            }
            Write-Host "  [INSTALL] instalando pacotes vcpkg em falta: $($installNames -join ', ')..." -ForegroundColor Magenta
            & $vcpkgExe install @installNames --triplet x64-windows
        } else {
            Write-Host ""
            Write-Host "  Para instalar os pacotes em falta:" -ForegroundColor Yellow
            $installNames = @()
            foreach ($name in $missingNames) {
                if ($name -eq "imgui") {
                    $installNames += "imgui[glfw-binding,opengl3-binding]"
                } else {
                    $installNames += $name
                }
            }
            $pkgList = $installNames -join " "
            Write-Host "  $vcpkgExe install $pkgList --triplet x64-windows" -ForegroundColor Yellow
        }
    }
} else {
    WARN "Pulando verificacao de pacotes - vcpkg nao encontrado"
}

# ---------------------------------------------------------------------------
Section "RUNTIME (dist/)"

$distRoot = "$PSScriptRoot\dist"

# Pasta onde o vcpkg deixa as DLLs compiladas (triplet x64-windows)
$vcpkgBin = $null
if ($vcpkgPath -and (Test-Path "$vcpkgPath\installed\x64-windows\bin")) {
    $vcpkgBin = "$vcpkgPath\installed\x64-windows\bin"
}

function CheckDistFiles($label, $dir, $files) {
    $missing = @()
    foreach ($f in $files) {
        if (-not (Test-Path (Join-Path $dir $f))) { $missing += $f }
    }
    if ($missing.Count -eq 0) {
        OK "$label : todos os arquivos de runtime presentes"
        return @()
    } else {
        FAIL "$label : faltando $($missing -join ', ')"
        return $missing
    }
}

function InstallMissingDlls($label, $dir, $missing) {
    if ($missing.Count -eq 0) { return }
    $dllsMissing = $missing | Where-Object { $_ -like "*.dll" }
    if ($dllsMissing.Count -eq 0) { return }
    if (-not $Install) { return }
    if (-not $vcpkgBin) {
        WARN "$label : nao foi possivel copiar DLLs - vcpkg bin nao encontrado ($vcpkgPath\installed\x64-windows\bin)"
        return
    }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    foreach ($dll in $dllsMissing) {
        $src = Join-Path $vcpkgBin $dll
        if (Test-Path $src) {
            Copy-Item $src (Join-Path $dir $dll) -Force
            OK "$label : copiado $dll de vcpkg"
        } else {
            WARN "$label : $dll nao encontrado em $vcpkgBin (compile o pacote correspondente primeiro)"
        }
    }
}

function CheckBuiltOutput($label, $file) {
    if (Test-Path $file) {
        OK "$label built: $file"
    } else {
        WARN "$label not built yet: $file"
    }
}

CheckBuiltOutput "client executable" "$distRoot\client\rco_client.exe"
$missingClient = CheckDistFiles "dist/client" "$distRoot\client" @(
    "config.toml",
    "assimp-vc143-mt.dll", "glfw3.dll", "kubazip.dll", "minizip.dll",
    "msquic.dll", "poly2tri.dll", "pugixml.dll", "zlib1.dll"
)
InstallMissingDlls "dist/client" "$distRoot\client" $missingClient

CheckBuiltOutput "GUE executable" "$distRoot\tools\rco_gue.exe"
$missingTools = CheckDistFiles "dist/tools (GUE)" "$distRoot\tools" @(
    "assimp-vc143-mt.dll", "glfw3.dll", "kubazip.dll", "minizip.dll",
    "poly2tri.dll", "pugixml.dll", "sqlite3.dll", "zlib1.dll"
)
InstallMissingDlls "dist/tools (GUE)" "$distRoot\tools" $missingTools

CheckBuiltOutput "server executable" "$distRoot\server\server.exe"
CheckDistFiles "dist/server" "$distRoot\server" @(
    "config.toml"
) | Out-Null

# Assets minimos para o cliente/GUE abrirem sem crashar (vem versionados em dist/ via git)
$missingAssets = CheckDistFiles "dist/client (shaders/ibl)" "$distRoot\client" @(
    "shaders\gBuffer.fs", "shaders\pbr_common.h", "assets\ibl\default.hdr"
)

$defaultAreaOk = Test-Path "$distRoot\client\data\areas\default\heightmap.bin"
if ($defaultAreaOk) {
    OK "area 'default' (terreno base) presente"
} else {
    WARN "area 'default' (data/areas/default/heightmap.bin) ausente - cliente pode nao ter terreno inicial"
}

if (($missingAssets.Count -gt 0 -or -not $defaultAreaOk) -and $Install -and $git) {
    Write-Host "  [INSTALL] restaurando assets/dados versionados de dist/ via git checkout..." -ForegroundColor Magenta
    git -C $PSScriptRoot checkout -- dist
    OK "dist/ restaurado do git (shaders, assets, data)"
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
