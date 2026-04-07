param(
    [Parameter(Mandatory = $true)]
    [string]$ShaderPath,

    [Parameter(Mandatory = $false)]
    [string]$OutPath = "",

    [Parameter(Mandatory = $false)]
    [string]$D3dxPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Relaunch-32BitIfNeeded {
    if ([IntPtr]::Size -ne 8) { return }
    $ps32 = Join-Path $env:WINDIR "SysWOW64\\WindowsPowerShell\\v1.0\\powershell.exe"
    if (-not (Test-Path $ps32)) { throw "32-bit PowerShell not found at: $ps32" }

    $args = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", $PSCommandPath,
        "-ShaderPath", $ShaderPath
    )
    if ($OutPath) { $args += @("-OutPath", $OutPath) }
    if ($D3dxPath) { $args += @("-D3dxPath", $D3dxPath) }

    & $ps32 @args
    exit $LASTEXITCODE
}

Relaunch-32BitIfNeeded

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$shaderFull = (Resolve-Path $ShaderPath).Path

if (-not $OutPath) {
    $OutPath = [System.IO.Path]::ChangeExtension($shaderFull, ".asm.txt")
}
$outFull = $OutPath
if (-not [System.IO.Path]::IsPathRooted($outFull)) {
    $outFull = Join-Path $root $outFull
}

if (-not $D3dxPath) {
    $candidate = Join-Path $root "Scene3D\\d3dx9_29.dll"
    if (Test-Path $candidate) {
        $D3dxPath = $candidate
    } else {
        $D3dxPath = "d3dx9_29.dll"
    }
}

$shaderBytes = [System.IO.File]::ReadAllBytes($shaderFull)

Add-Type -Language CSharp -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;

public static class D3D9ShaderDisasm
{
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
    private static extern IntPtr LoadLibraryA(string lpFileName);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr hModule, string lpProcName);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FreeLibrary(IntPtr hModule);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int D3DXDisassembleShaderDelegate(
        IntPtr pShader,
        int enableColorCode,
        IntPtr pComments,
        out IntPtr ppDisassembly);

    [ComImport]
    [Guid("8BA5FB08-5195-40E2-AC58-0D989C3A0102")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface ID3DXBuffer
    {
        [PreserveSig] IntPtr GetBufferPointer();
        [PreserveSig] int GetBufferSize();
    }

    public static string Disassemble(string d3dxPath, byte[] shaderBytecode)
    {
        if (shaderBytecode == null || shaderBytecode.Length < 4)
            throw new ArgumentException("Shader bytecode is empty.");

        IntPtr mod = LoadLibraryA(d3dxPath);
        if (mod == IntPtr.Zero)
            throw new Exception("LoadLibrary failed for: " + d3dxPath);

        try
        {
            IntPtr proc = GetProcAddress(mod, "D3DXDisassembleShader");
            if (proc == IntPtr.Zero)
                throw new Exception("GetProcAddress failed for D3DXDisassembleShader in: " + d3dxPath);

            var disasm = (D3DXDisassembleShaderDelegate)Marshal.GetDelegateForFunctionPointer(
                proc, typeof(D3DXDisassembleShaderDelegate));

            GCHandle h = GCHandle.Alloc(shaderBytecode, GCHandleType.Pinned);
            try
            {
                IntPtr bufPtr;
                int hr = disasm(h.AddrOfPinnedObject(), 0, IntPtr.Zero, out bufPtr);
                if (hr < 0 || bufPtr == IntPtr.Zero)
                    throw new Exception("D3DXDisassembleShader failed: HRESULT=0x" + hr.ToString("X8"));

                object obj = Marshal.GetObjectForIUnknown(bufPtr);
                try
                {
                    var buf = (ID3DXBuffer)obj;
                    int size = buf.GetBufferSize();
                    IntPtr p = buf.GetBufferPointer();
                    if (p == IntPtr.Zero || size <= 0)
                        return "";

                    byte[] bytes = new byte[size];
                    Marshal.Copy(p, bytes, 0, size);
                    return System.Text.Encoding.ASCII.GetString(bytes);
                }
                finally
                {
                    Marshal.ReleaseComObject(obj);
                }
            }
            finally
            {
                if (h.IsAllocated) h.Free();
            }
        }
        finally
        {
            FreeLibrary(mod);
        }
    }
}
"@

$asm = [D3D9ShaderDisasm]::Disassemble($D3dxPath, $shaderBytes)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($outFull)) | Out-Null
[System.IO.File]::WriteAllText($outFull, $asm)

Write-Host ("Wrote disassembly: {0}" -f $outFull)
