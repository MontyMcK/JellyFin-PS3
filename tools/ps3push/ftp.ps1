# Push an EBOOT to the PS3 over webMAN's FTP, and prove it landed.
#
# webMAN's server is anonymous, passive, and speaks latin-1 -- which in Windows
# PowerShell 5.1 must be named by codepage (28591); the string 'latin-1' is not
# a valid encoding name there.
#
#   .\ftp.ps1 -Local <path> [-Remote <path>] [-Ip <addr>] [-NoUpload]
#
# Always reads the file back afterwards and compares SHA256, because a short
# write over FTP looks exactly like a successful one.

param(
    [Parameter(Mandatory = $true)][string]$Local,
    [string]$Remote = '/dev_hdd0/game/JFPS30000/USRDIR/EBOOT.BIN',
    [string]$Ip     = '192.168.0.202',
    [string]$OutDir = $PSScriptRoot,
    [switch]$NoUpload
)

$ErrorActionPreference = 'Stop'
$uri = "ftp://$Ip$Remote"

function New-FtpRequest([string]$u, [string]$method) {
    $r = [System.Net.FtpWebRequest]::Create($u)
    $r.Method      = $method
    $r.Credentials = New-Object System.Net.NetworkCredential('anonymous', 'anonymous@')
    $r.UseBinary   = $true
    $r.UsePassive  = $true
    $r.KeepAlive   = $false
    $r.Timeout     = 30000
    $r.ReadWriteTimeout = 60000
    return $r
}

function Get-FtpInfo([string]$u) {
    $size = $null; $stamp = $null
    try {
        $r = New-FtpRequest $u ([System.Net.WebRequestMethods+Ftp]::GetFileSize)
        $resp = $r.GetResponse(); $size = $resp.ContentLength; $resp.Close()
    } catch { }
    try {
        $r = New-FtpRequest $u ([System.Net.WebRequestMethods+Ftp]::GetDateTimestamp)
        $resp = $r.GetResponse(); $stamp = $resp.LastModified; $resp.Close()
    } catch { }
    return [pscustomobject]@{ Size = $size; Modified = $stamp }
}

function Get-FtpFile([string]$u, [string]$out) {
    $r = New-FtpRequest $u ([System.Net.WebRequestMethods+Ftp]::DownloadFile)
    $resp = $r.GetResponse()
    $in = $resp.GetResponseStream()
    $fs = [System.IO.File]::Create($out)
    try { $in.CopyTo($fs) } finally { $fs.Close(); $in.Close(); $resp.Close() }
}

function Send-FtpFile([string]$u, [string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $r = New-FtpRequest $u ([System.Net.WebRequestMethods+Ftp]::UploadFile)
    $r.ContentLength = $bytes.Length
    $s = $r.GetRequestStream()
    try { $s.Write($bytes, 0, $bytes.Length) } finally { $s.Close() }
    $resp = $r.GetResponse()
    $status = "$($resp.StatusCode) $($resp.StatusDescription)".Trim()
    $resp.Close()
    return $status
}

function Sha([string]$path) { (Get-FileHash -Algorithm SHA256 $path).Hash.ToLower() }

$localSha = Sha $Local
$localLen = (Get-Item $Local).Length
Write-Output "local    : $Local"
Write-Output "           $localLen bytes  $localSha"

$before = Get-FtpInfo $uri
Write-Output "remote   : $uri"
Write-Output "           before: $($before.Size) bytes, modified $($before.Modified)"

$backup = Join-Path $OutDir 'EBOOT.console-before.bin'
Get-FtpFile $uri $backup
Write-Output "backup   : $backup  $(Sha $backup)"

if ($NoUpload) { Write-Output 'upload   : SKIPPED (-NoUpload)'; exit 0 }

$status = Send-FtpFile $uri $Local
Write-Output "upload   : $status"

$readback = Join-Path $OutDir 'EBOOT.console-after.bin'
Get-FtpFile $uri $readback
$rbSha = Sha $readback
$after = Get-FtpInfo $uri
Write-Output "readback : $((Get-Item $readback).Length) bytes  $rbSha"
Write-Output "           after: $($after.Size) bytes, modified $($after.Modified)"
if ($rbSha -eq $localSha) { Write-Output 'VERIFY   : MATCH' }
else                      { Write-Output 'VERIFY   : MISMATCH'; exit 1 }
