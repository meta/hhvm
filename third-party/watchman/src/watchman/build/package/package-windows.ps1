# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Usage: package-windows.ps1 BUILD_DIR RELEASE OUT_DIR
#
# Packages a Windows build as watchman-RELEASE-windows.zip in OUT_DIR. Its bin
# folder holds the executables and a Python for the Python tools. Run it in a
# developer environment, which puts dumpbin on PATH.
param(
    [Parameter(Mandatory)] [string] $Build,
    [Parameter(Mandatory)] [string] $Release,
    [Parameter(Mandatory)] [string] $Out
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# python.org's embeddable package, with the SHA-256 that its release page lists.
$python = [version]'3.14.10'
$pythonSha256 = '90905e64a840557d8c1bc4f5b74db3d7ee85e3ffb217611cac1c7813acea4159'

$name = "watchman-$Release-windows"
$tmp = Join-Path -Path ([IO.Path]::GetTempPath()) -ChildPath ([IO.Path]::GetRandomFileName())
$bin = "$tmp\$name\bin"
New-Item -ItemType Directory -Force -Path $bin, $Out | Out-Null
try {
    # Scoop, winget and Chocolatey refer to these by name.
    $exes = @(
        'bin\watchman.exe',
        'watchman\cli\release\watchmanctl.exe',
        'watchman\thirdparty\deelevate_binding\release\eledo-pty-bridge.exe',
        'watchman\python\watchman-diag.exe',
        'watchman\python\watchman-make.exe',
        'watchman\python\watchman-replicate-subscription.exe',
        'watchman\python\watchman-wait.exe'
    )
    foreach ($exe in $exes) {
        Copy-Item -Path "$Build\$exe" -Destination $bin
    }

    # The Python tools load python3.dll from their own folder. It forwards to
    # pythonXY.dll, which reads its search path from the pythonXY._pth next to
    # it and imports the package's vcruntime DLLs. The rest goes in bin\python,
    # off PATH, without python.exe, which would shadow the user's own Python.
    $py = "python$($python.Major)$($python.Minor)"
    $embed = "$tmp\python-$python-embed-amd64.zip"
    Invoke-WebRequest -Uri "https://www.python.org/ftp/python/$python/python-$python-embed-amd64.zip" -OutFile $embed
    if ((Get-FileHash -Path $embed -Algorithm SHA256).Hash -ne $pythonSha256) {
        throw "$embed doesn't have the expected SHA-256"
    }
    Expand-Archive -Path $embed -DestinationPath "$bin\python"
    Move-Item -Path "$bin\python\python3.dll", "$bin\python\$py.dll", "$bin\python\vcruntime*.dll" -Destination $bin
    Remove-Item -Path "$bin\python\$py._pth", "$bin\python\python*.exe"
    Set-Content -Path "$bin\$py._pth" -Value "python\$py.zip", 'python'

    # Every DLL that bin imports must be in bin or in System32, which has
    # Windows' own DLLs and those of the Visual C++ Redistributable that
    # install.md asks for.
    foreach ($file in Get-ChildItem -File $bin) {
        $imports = dumpbin /nologo /dependents $file.FullName |
            ForEach-Object { if ($_ -match '^\s+(\S+\.dll)\s*$') { $Matches[1] } }
        foreach ($dll in $imports) {
            if ($dll -notlike 'api-ms-win-*' -and -not (Test-Path "$bin\$dll") -and
                -not (Test-Path "$env:SystemRoot\System32\$dll")) {
                throw "$($file.Name) imports $dll, which is neither in bin nor in System32"
            }
        }
    }

    $version = & "$bin\watchman.exe" --version
    if ($version -eq '0.0.0') {
        throw 'watchman has no version; build from a git checkout'
    }

    # The launcher looks for python3.dll only in bin and System32, so these run
    # on the Python in bin.
    foreach ($tool in 'watchman-make', 'watchman-replicate-subscription', 'watchman-wait') {
        & "$bin\$tool.exe" --help | Out-Null
        if ($LASTEXITCODE) {
            throw "$tool.exe --help exited with $LASTEXITCODE"
        }
    }

    Compress-Archive -Force -Path "$tmp\$name" -DestinationPath "$Out\$name.zip"
    Get-ChildItem $bin | Format-Table -Property Name, Length
    Get-ChildItem $Out
} finally {
    Remove-Item -Recurse -Force -Path $tmp
}
