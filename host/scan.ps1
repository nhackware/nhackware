# host/scan.ps1 - locate the guest RAM of the BlueStacks VM inside the emulator
# process on Windows. No root needed here.
#
# The guest-side nhext (page mode) dumps two 4KB game pages plus their
# guest-physical addresses (pa). We scan the emulator process's big writable
# regions for those exact pages. If the guest RAM is linearly mapped, both pages
# yield the same delta (hostVA - pa), which is the guest-physical -> host-virtual
# translation base for everything that follows.
#
#   powershell -ExecutionPolicy Bypass -File host\scan.ps1 -Pa0 0x1234 -Pa1 0x5678

param(
    [string[]]$Procs = @("BstkSVC", "HD-Player", "HD-VMSvc", "BlueStacks"),
    [string]$Page0 = "page0.bin",
    [string]$Page1 = "page1.bin",
    [long]$Pa0 = -1,
    [long]$Pa1 = -1
)

$src = @"
using System;
using System.Runtime.InteropServices;
public class N {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int n, out int read);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern int VirtualQueryEx(IntPtr h, IntPtr addr, out MBI mbi, int len);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr h);
    [StructLayout(LayoutKind.Sequential)]
    public struct MBI {
        public IntPtr BaseAddress;
        public IntPtr AllocationBase;
        public uint AllocationProtect;
        public IntPtr RegionSize;
        public uint State;
        public uint Protect;
        public uint Type;
    }
}
"@
Add-Type -TypeDefinition $src

$MEM_COMMIT = 0x1000
$RW = @(0x04, 0x08, 0x40, 0x80)   # READWRITE, WRITECOPY, EXEC_READWRITE, EXEC_WRITECOPY

$p0 = [System.IO.File]::ReadAllBytes($Page0)
$p1 = [System.IO.File]::ReadAllBytes($Page1)

function Find-Page([IntPtr]$h, [byte[]]$pat) {
    $addr = [IntPtr]::Zero
    $buf = New-Object byte[] (4MB)
    while ($true) {
        $mbi = New-Object N+MBI
        $r = [N]::VirtualQueryEx($h, $addr, [ref]$mbi, [System.Runtime.InteropServices.Marshal]::SizeOf($mbi))
        if ($r -eq 0) { return -1 }
        $next = [long]$mbi.BaseAddress + [long]$mbi.RegionSize
        if ($mbi.State -eq $MEM_COMMIT -and ($RW -contains $mbi.Protect) -and [long]$mbi.RegionSize -ge 64MB) {
            $off = 0
            $size = [long]$mbi.RegionSize
            while ($off -lt $size) {
                $chunk = [int][Math]::Min([long]$buf.Length, $size - $off)
                $read = 0
                $base = [long]$mbi.BaseAddress + $off
                if (-not [N]::ReadProcessMemory($h, [IntPtr]$base, $buf, $chunk, [ref]$read)) { break }
                $idx = 0
                while (($idx = [Array]::IndexOf($buf, $pat[0], $idx, $read - $idx)) -ge 0) {
                    if ($read - $idx -ge 16) {
                        $ok = $true
                        for ($i = 0; $i -lt 16; $i++) { if ($buf[$idx + $i] -ne $pat[$i]) { $ok = $false; break } }
                        if ($ok) {
                            $full = New-Object byte[] 4096
                            $fr = 0
                            if ([N]::ReadProcessMemory($h, [IntPtr]($base + $idx), $full, 4096, [ref]$fr)) {
                                $ok2 = $true
                                for ($i = 0; $i -lt 4096; $i++) { if ($full[$i] -ne $pat[$i]) { $ok2 = $false; break } }
                                if ($ok2) { return ($base + $idx) }
                            }
                        }
                    }
                    $idx++
                }
                $off += $chunk
            }
        }
        $addr = [IntPtr]$next
        if ([long]$addr -eq 0) { return -1 }   # wrapped address space end
    }
}

foreach ($name in $Procs) {
    $ps = Get-Process -Name $name -ErrorAction SilentlyContinue
    foreach ($p in $ps) {
        $h = [N]::OpenProcess(0x0010 -bor 0x0400, $false, $p.Id)   # VM_READ | QUERY_INFORMATION
        if ($h -eq [IntPtr]::Zero) { continue }
        Write-Host "[scan] $name pid=$($p.Id) scanning..."
        $va0 = Find-Page $h $p0
        if ($va0 -ge 0) {
            $va1 = Find-Page $h $p1
            Write-Host "[scan] FOUND in $name pid=$($p.Id)"
            Write-Host ("[scan] page0 hostVA 0x{0:x}" -f $va0)
            Write-Host ("[scan] page1 hostVA 0x{0:x}" -f $va1)
            if ($Pa0 -ge 0) { Write-Host ("[scan] delta0 0x{0:x}" -f ($va0 - $Pa0)) }
            if ($Pa1 -ge 0) { Write-Host ("[scan] delta1 0x{0:x}" -f ($va1 - $Pa1)) }
            [void][N]::CloseHandle($h)
            exit 0
        }
        [void][N]::CloseHandle($h)
    }
}
Write-Host "[scan] not found in any process"
exit 1
