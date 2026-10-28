# pi/circle/sidecar-reboot.ps1 - reboots a Sidecar (Pi 4B) over its USB-C MIDI port, Windows: the reboot SysEx of
# pi/engine/reboot_sysex.h, obeyed only with usbreboot=1 in cmdline.txt. With the rpiboot GPIO held low and an rpiboot
# loop on the computer (docs: pi4-rpiboot), the Pi boots again from the loop's folder. Needs no network.
#   powershell -ExecutionPolicy Bypass -File sidecar-reboot.ps1 [-Port "Sidecar"]
# -Port: a part of the MIDI output's name (default "Sidecar"); the first match is used. Close apps holding the port
# exclusively first (Windows MIDI ports are single-client).
param([string]$Port = "Sidecar")

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class WinMM {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct MIDIOUTCAPS {
        public ushort wMid, wPid; public uint vDriverVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szPname;
        public ushort wTechnology, wVoices, wNotes, wChannelMask; public uint dwSupport;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct MIDIHDR {
        public IntPtr lpData; public uint dwBufferLength, dwBytesRecorded; public IntPtr dwUser; public uint dwFlags;
        public IntPtr lpNext, reserved; public uint dwOffset;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] public IntPtr[] dwReserved;
    }
    [DllImport("winmm.dll")] public static extern uint midiOutGetNumDevs();
    [DllImport("winmm.dll", CharSet = CharSet.Unicode)] public static extern uint midiOutGetDevCaps(UIntPtr id, ref MIDIOUTCAPS caps, uint size);
    [DllImport("winmm.dll")] public static extern uint midiOutOpen(out IntPtr h, uint id, IntPtr cb, IntPtr inst, uint flags);
    [DllImport("winmm.dll")] public static extern uint midiOutPrepareHeader(IntPtr h, IntPtr hdr, uint size);
    [DllImport("winmm.dll")] public static extern uint midiOutLongMsg(IntPtr h, IntPtr hdr, uint size);
    [DllImport("winmm.dll")] public static extern uint midiOutUnprepareHeader(IntPtr h, IntPtr hdr, uint size);
    [DllImport("winmm.dll")] public static extern uint midiOutClose(IntPtr h);
}
"@

# F0 7D 52 54 7E "REBOOT" F7 (reboot_sysex.h)
[byte[]]$Msg = 0xF0, 0x7D, 0x52, 0x54, 0x7E, 0x52, 0x45, 0x42, 0x4F, 0x4F, 0x54, 0xF7

$Id = -1
for ($i = 0; $i -lt [WinMM]::midiOutGetNumDevs(); $i++) {
    $c = New-Object WinMM+MIDIOUTCAPS
    [void][WinMM]::midiOutGetDevCaps([UIntPtr][uint32]$i, [ref]$c, [Runtime.InteropServices.Marshal]::SizeOf($c))
    if ($Id -lt 0 -and $c.szPname -like "*$Port*") { $Id = $i; $Name = $c.szPname }
}
if ($Id -lt 0) { Write-Error "no MIDI output matching '$Port'"; exit 1 }

$h = [IntPtr]::Zero
$r = [WinMM]::midiOutOpen([ref]$h, [uint32]$Id, [IntPtr]::Zero, [IntPtr]::Zero, 0)
if ($r -ne 0) { Write-Error "midiOutOpen '$Name' failed ($r): is the port open in another app?"; exit 1 }
$Data = [Runtime.InteropServices.Marshal]::AllocHGlobal($Msg.Length)
[Runtime.InteropServices.Marshal]::Copy($Msg, 0, $Data, $Msg.Length)
$Hdr = New-Object WinMM+MIDIHDR
$Hdr.lpData = $Data; $Hdr.dwBufferLength = $Msg.Length; $Hdr.dwBytesRecorded = $Msg.Length
$Hdr.dwReserved = New-Object IntPtr[] 8
$Size = [Runtime.InteropServices.Marshal]::SizeOf($Hdr)
$P = [Runtime.InteropServices.Marshal]::AllocHGlobal($Size)
[Runtime.InteropServices.Marshal]::StructureToPtr($Hdr, $P, $false)
[void][WinMM]::midiOutPrepareHeader($h, $P, $Size)
$r = [WinMM]::midiOutLongMsg($h, $P, $Size)
Start-Sleep -Milliseconds 200                 # the SysEx out before the header goes
[void][WinMM]::midiOutUnprepareHeader($h, $P, $Size)
[void][WinMM]::midiOutClose($h)
[Runtime.InteropServices.Marshal]::FreeHGlobal($P)
[Runtime.InteropServices.Marshal]::FreeHGlobal($Data)
if ($r -ne 0) { Write-Error "midiOutLongMsg failed ($r)"; exit 1 }
Write-Output "reboot SysEx sent to '$Name' (the Pi reboots only with usbreboot=1)"
