# Installs WinUSB for the KeyStep mk2's firmware-updater interface and for its update mode, so
# tools/ksflash.py (libusb) can reach them. The MIDI interface keeps Windows' own driver.
# Run from an elevated PowerShell: powershell -ExecutionPolicy Bypass -File install-winusb.ps1
# It signs the package with a certificate made here and trusts that certificate on this computer.
$ErrorActionPreference = "Stop"
$dir = $PSScriptRoot
$inf = Join-Path $dir "keystep-winusb.inf"
$cat = Join-Path $dir "keystep-winusb.cat"

$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq "CN=keystep-winusb" } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=keystep-winusb" -CertStoreLocation Cert:\LocalMachine\My
    foreach ($name in "Root", "TrustedPublisher") {
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store $name, "LocalMachine"
        $store.Open("ReadWrite"); $store.Add($cert); $store.Close()
    }
}
Remove-Item -ErrorAction SilentlyContinue $cat
New-FileCatalog -Path $inf -CatalogFilePath $cat -CatalogVersion 2.0 | Out-Null
$signed = Set-AuthenticodeSignature -FilePath $cat -Certificate $cert
if ($signed.Status -ne "Valid") { throw "signing failed: $($signed.StatusMessage)" }
pnputil /add-driver $inf /install
