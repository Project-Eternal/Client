#!/usr/bin/env python3
"""Writes d3d9.def (the methods of the Direct3D 9 interfaces as procedures, for gen_glue.py) and ../src/d3d9-methods.inc (the
vtables of the interfaces, in the order Windows has them, for d3d9-bridge.cpp).

A method is a stdcall procedure whose first argument is the object; the number after its name counts all the arguments."""
import os

IUNKNOWN = [("QueryInterface", 3), ("AddRef", 1), ("Release", 1)]

RESOURCE = [("GetDevice", 2), ("SetPrivateData", 5), ("GetPrivateData", 4), ("FreePrivateData", 2), ("SetPriority", 2),
            ("GetPriority", 1), ("PreLoad", 1), ("GetType", 1)]

BASETEXTURE = [("SetLOD", 2), ("GetLOD", 1), ("GetLevelCount", 1), ("SetAutoGenFilterType", 2), ("GetAutoGenFilterType", 1),
               ("GenerateMipSubLevels", 1)]

INTERFACES = {
    "IDirect3D9": IUNKNOWN + [
        ("RegisterSoftwareDevice", 2), ("GetAdapterCount", 1), ("GetAdapterIdentifier", 4), ("GetAdapterModeCount", 3),
        ("EnumAdapterModes", 5), ("GetAdapterDisplayMode", 3), ("CheckDeviceType", 6), ("CheckDeviceFormat", 7),
        ("CheckDeviceMultiSampleType", 7), ("CheckDepthStencilMatch", 6), ("CheckDeviceFormatConversion", 5),
        ("GetDeviceCaps", 4), ("GetAdapterMonitor", 2), ("CreateDevice", 7)],
    "IDirect3DDevice9": IUNKNOWN + [
        ("TestCooperativeLevel", 1), ("GetAvailableTextureMem", 1), ("EvictManagedResources", 1), ("GetDirect3D", 2),
        ("GetDeviceCaps", 2), ("GetDisplayMode", 3), ("GetCreationParameters", 2), ("SetCursorProperties", 4),
        ("SetCursorPosition", 4), ("ShowCursor", 2), ("CreateAdditionalSwapChain", 3), ("GetSwapChain", 3),
        ("GetNumberOfSwapChains", 1), ("Reset", 2), ("Present", 5), ("GetBackBuffer", 5), ("GetRasterStatus", 3),
        ("SetDialogBoxMode", 2), ("SetGammaRamp", 4), ("GetGammaRamp", 3), ("CreateTexture", 9), ("CreateVolumeTexture", 10),
        ("CreateCubeTexture", 8), ("CreateVertexBuffer", 7), ("CreateIndexBuffer", 7), ("CreateRenderTarget", 9),
        ("CreateDepthStencilSurface", 9), ("UpdateSurface", 5), ("UpdateTexture", 3), ("GetRenderTargetData", 3),
        ("GetFrontBufferData", 3), ("StretchRect", 6), ("ColorFill", 4), ("CreateOffscreenPlainSurface", 7),
        ("SetRenderTarget", 3), ("GetRenderTarget", 3), ("SetDepthStencilSurface", 2), ("GetDepthStencilSurface", 2),
        ("BeginScene", 1), ("EndScene", 1), ("Clear", 7), ("SetTransform", 3), ("GetTransform", 3), ("MultiplyTransform", 3),
        ("SetViewport", 2), ("GetViewport", 2), ("SetMaterial", 2), ("GetMaterial", 2), ("SetLight", 3), ("GetLight", 3),
        ("LightEnable", 3), ("GetLightEnable", 3), ("SetClipPlane", 3), ("GetClipPlane", 3), ("SetRenderState", 3),
        ("GetRenderState", 3), ("CreateStateBlock", 3), ("BeginStateBlock", 1), ("EndStateBlock", 2), ("SetClipStatus", 2),
        ("GetClipStatus", 2), ("GetTexture", 3), ("SetTexture", 3), ("GetTextureStageState", 4), ("SetTextureStageState", 4),
        ("GetSamplerState", 4), ("SetSamplerState", 4), ("ValidateDevice", 2), ("SetPaletteEntries", 3),
        ("GetPaletteEntries", 3), ("SetCurrentTexturePalette", 2), ("GetCurrentTexturePalette", 2), ("SetScissorRect", 2),
        ("GetScissorRect", 2), ("SetSoftwareVertexProcessing", 2), ("GetSoftwareVertexProcessing", 1), ("SetNPatchMode", 2),
        ("GetNPatchMode", 1), ("DrawPrimitive", 4), ("DrawIndexedPrimitive", 7), ("DrawPrimitiveUP", 5),
        ("DrawIndexedPrimitiveUP", 9), ("ProcessVertices", 7), ("CreateVertexDeclaration", 3), ("SetVertexDeclaration", 2),
        ("GetVertexDeclaration", 2), ("SetFVF", 2), ("GetFVF", 2), ("CreateVertexShader", 3), ("SetVertexShader", 2),
        ("GetVertexShader", 2), ("SetVertexShaderConstantF", 4), ("GetVertexShaderConstantF", 4),
        ("SetVertexShaderConstantI", 4), ("GetVertexShaderConstantI", 4), ("SetVertexShaderConstantB", 4),
        ("GetVertexShaderConstantB", 4), ("SetStreamSource", 5), ("GetStreamSource", 5), ("SetStreamSourceFreq", 3),
        ("GetStreamSourceFreq", 3), ("SetIndices", 2), ("GetIndices", 2), ("CreatePixelShader", 3), ("SetPixelShader", 2),
        ("GetPixelShader", 2), ("SetPixelShaderConstantF", 4), ("GetPixelShaderConstantF", 4),
        ("SetPixelShaderConstantI", 4), ("GetPixelShaderConstantI", 4), ("SetPixelShaderConstantB", 4),
        ("GetPixelShaderConstantB", 4), ("DrawRectPatch", 4), ("DrawTriPatch", 4), ("DeletePatch", 2), ("CreateQuery", 3)],
    "IDirect3DTexture9": IUNKNOWN + RESOURCE + BASETEXTURE + [
        ("GetLevelDesc", 3), ("GetSurfaceLevel", 3), ("LockRect", 5), ("UnlockRect", 2), ("AddDirtyRect", 2)],
    "IDirect3DSurface9": IUNKNOWN + RESOURCE + [
        ("GetContainer", 3), ("GetDesc", 2), ("LockRect", 4), ("UnlockRect", 1), ("GetDC", 2), ("ReleaseDC", 2)],
    "IDirect3DVertexBuffer9": IUNKNOWN + RESOURCE + [("Lock", 5), ("Unlock", 1), ("GetDesc", 2)],
    "IDirect3DIndexBuffer9": IUNKNOWN + RESOURCE + [("Lock", 5), ("Unlock", 1), ("GetDesc", 2)],
    "IDirect3DVertexDeclaration9": IUNKNOWN + [("GetDevice", 2), ("GetDeclaration", 3)],
    "IDirect3DVertexShader9": IUNKNOWN + [("GetDevice", 2), ("GetFunction", 3)],
    "IDirect3DPixelShader9": IUNKNOWN + [("GetDevice", 2), ("GetFunction", 3)],
    "IDirect3DStateBlock9": IUNKNOWN + [("GetDevice", 2), ("Capture", 1), ("Apply", 1)],
    "IDirect3DQuery9": IUNKNOWN + [("GetDevice", 2), ("GetType", 1), ("GetDataSize", 1), ("Issue", 2), ("GetData", 4)],
}

# the first definition of a method is the one that has a name; the methods a resource has in common are shared
def procedure_name(interface, index, method):
    first = index < 3
    if first:
        return "IUnknown_" + method
    if interface.endswith("Texture9") and 3 <= index < 3 + len(RESOURCE):
        return "IDirect3DResource9_" + method
    if interface in ("IDirect3DSurface9", "IDirect3DVertexBuffer9", "IDirect3DIndexBuffer9") and 3 <= index < 3 + len(RESOURCE):
        return "IDirect3DResource9_" + method
    if interface == "IDirect3DTexture9" and 3 + len(RESOURCE) <= index < 3 + len(RESOURCE) + len(BASETEXTURE):
        return "IDirect3DBaseTexture9_" + method
    return interface + "_" + method


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    procs = {}
    tables = []
    for interface, methods in INTERFACES.items():
        names = []
        for i, (method, nargs) in enumerate(methods):
            name = procedure_name(interface, i, method)
            if name in procs and procs[name] != nargs:
                raise SystemExit("%s has two argument counts" % name)
            procs[name] = nargs
            names.append(name)
        tables.append((interface, names))
    with open(os.path.join(root, "recompiler", "d3d9.def"), "w") as f:
        f.write("# generated by recompiler/make_d3d9_def.py: the methods of the Direct3D 9 interfaces (same format as imports.def)\n")
        for name, nargs in procs.items():
            f.write("%s d3d9 %d %s\n" % (name, nargs, " ".join("i" * nargs)))
    with open(os.path.join(root, "src", "d3d9-methods.inc"), "w") as f:
        f.write("/* generated by recompiler/make_d3d9_def.py: the vtables of the interfaces, in the order Windows has them */\n")
        for interface, names in tables:
            f.write("D3D_VTABLE(%s,\n" % interface)
            f.write(",\n".join('    "%s"' % n for n in names))
            f.write(")\n")
    print("%d procedures, %d interfaces" % (len(procs), len(tables)))


if __name__ == "__main__":
    main()
