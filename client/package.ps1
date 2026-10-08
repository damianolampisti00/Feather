# Builds the Feather client and packages a .bar, bumping <buildId> in
# bar-descriptor.xml first so BB10 accepts it as an in-place update (the
# version must increase).
#
# Usage:  powershell -File package.ps1
#         powershell -File package.ps1 -Install    (also copies it to a ROOTED
#                     phone and installs it there over SSH, see tools/installbar.sh)
#         powershell -File package.ps1 -Install -PhoneIp 192.168.1.xxx -RootKey C:\path\to\id_rsa
#
# -PhoneIp / -RootKey default to $PhoneIp / $RootKey from package.config.ps1
# next to this script (not in git; copy package.config.example.ps1).
#
# Needs the BlackBerry 10 Native SDK 10.3 in C:\bbndk (Momentics' default) and,
# for -Install, Git for Windows. The NDK's own JRE 1.7 runs the packager (a
# newer system Java crashes it).
param(
    [switch]$Install,
    [string]$PhoneIp,
    [string]$RootKey
)
$ErrorActionPreference = 'Stop'
$c = $PSScriptRoot
$config = Join-Path $c 'package.config.ps1'
if (Test-Path $config) {
    $cliPhoneIp = $PhoneIp; $cliRootKey = $RootKey
    . $config
    if ($cliPhoneIp) { $PhoneIp = $cliPhoneIp }
    if ($cliRootKey) { $RootKey = $cliRootKey }
}
$h = 'C:\bbndk\ndk\host_10_3_1_12\win32\x86'
$jre = 'C:\bbndk\features\com.qnx.tools.jre.win32.x86_64_1.7.0.51\jre'
$env:QNX_HOST = $h
$env:QNX_TARGET = 'C:\bbndk\ndk\target_10_3_1_995\qnx6'
$env:JAVA_HOME = $jre
$env:PATH = "$jre\bin;$h\usr\bin;$env:PATH"

# --- bump build id ---
$desc = Get-Content "$c\bar-descriptor.xml" -Raw
$build = [int]([regex]::Match($desc, '<buildId>(\d+)</buildId>').Groups[1].Value) + 1
$desc = $desc -replace '<buildId>\d+</buildId>', "<buildId>$build</buildId>"
$ver = [regex]::Match($desc, '<versionNumber>([^<]+)</versionNumber>').Groups[1].Value
Set-Content "$c\bar-descriptor.xml" $desc -Encoding UTF8 -NoNewline
$full = "$ver.$build"

# --- translations: translations\*.ts -> assets\translations\*.qm ---
$lrelease = "$h\usr\bin\lrelease.exe"
if (Test-Path $lrelease) {
    New-Item -ItemType Directory -Force "$c\assets\translations" | Out-Null
    foreach ($ts in Get-ChildItem "$c\translations\*.ts") {
        & $lrelease -silent $ts.FullName -qm "$c\assets\translations\$($ts.BaseName).qm"
        if ($LASTEXITCODE -ne 0) { throw "lrelease failed for $($ts.Name)" }
    }
}

# --- build (out of tree; delete build\Makefile after a .pro change) ---
New-Item -ItemType Directory -Force "$c\build" | Out-Null
Push-Location "$c\build"
if (-not (Test-Path Makefile)) {
    qmake ..\Feather.pro -spec blackberry-armv7le-qcc CONFIG+=device CONFIG+=release
}
make -j4
if ($LASTEXITCODE -ne 0) { Pop-Location; throw 'make failed' }
Pop-Location

# --- stage + package ---
$s = "$c\stage"
Remove-Item $s -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$s\arm\o.le-v7" | Out-Null
Copy-Item "$c\build\o.le-v7\Feather.so" "$s\arm\o.le-v7\"
Copy-Item "$c\assets" "$s\assets" -Recurse
Copy-Item "$c\icon.png", "$c\bar-descriptor.xml" $s
Push-Location $s
$bar = "$c\Feather-$full-armv7.bar"
blackberry-nativepackager -package $bar bar-descriptor.xml -configuration Device-Release -devMode
$packOk = ($LASTEXITCODE -eq 0)
Pop-Location
if (-not $packOk) { throw 'packaging failed' }
Write-Host "Created $bar (version $full)"

if (-not $Install) { exit 0 }
if (-not $PhoneIp -or -not $RootKey) { throw '-Install needs -PhoneIp and -RootKey (or package.config.ps1)' }

# --- copy to the phone and install, over SSH as root (see tools/installbar.sh) ---
# Done through Git Bash, not PowerShell's own ssh/scp: PowerShell's scp reads
# "C:\..." (a colon before the first slash) as a remote "host:path", and piping
# a binary file through PowerShell's pipeline to a native process can corrupt
# it. Git Bash's `<` redirection has neither problem.
$barName = Split-Path $bar -Leaf
$installer = Join-Path (Split-Path $c) 'tools\installbar.sh'
$toPosix = { param($w) '/' + $w.Substring(0, 1).ToLower() + $w.Substring(2).Replace('\', '/') }
$barPosix = & $toPosix $bar
$installerPosix = & $toPosix $installer
$keyPosix = & $toPosix $RootKey

$bashScript = @"
set -e
# BB10's sshd only offers old algorithms: they have to be allowed explicitly.
SSH=(ssh -i "$keyPosix" -o HostKeyAlgorithms=ssh-rsa -o PubkeyAcceptedAlgorithms=ssh-rsa -o KexAlgorithms=diffie-hellman-group14-sha1 -o Ciphers=aes128-cbc -o MACs=hmac-sha1 -o StrictHostKeyChecking=no -o ConnectTimeout=10 root@$PhoneIp)
echo "Copying $barName and installbar.sh to the phone ($PhoneIp)..."
`"`${SSH[@]}`" "cat > /accounts/1000/shared/downloads/$barName" < "$barPosix"
`"`${SSH[@]}`" "cat > /accounts/1000/shared/downloads/installbar.sh" < "$installerPosix"
echo "Installing..."
`"`${SSH[@]}`" "cat /accounts/1000/shared/downloads/installbar.sh | sh -s -- $barName"
"@
# A plain file, not a pipe: PowerShell's pipe-to-native-stdin encoding adds a
# UTF-8 BOM that garbles bash's first line.
$scriptFile = Join-Path $env:TEMP 'feather_install.sh'
[System.IO.File]::WriteAllText($scriptFile, $bashScript.Replace("`r`n", "`n"), (New-Object System.Text.UTF8Encoding $false))
& 'C:\Program Files\Git\bin\bash.exe' (& $toPosix $scriptFile)
if ($LASTEXITCODE -ne 0) { throw "install over SSH failed (exit $LASTEXITCODE) -- is the phone reachable at $PhoneIp?" }
Remove-Item $scriptFile -ErrorAction SilentlyContinue
Write-Host "Installed $barName on the phone."
