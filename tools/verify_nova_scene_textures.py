#!/usr/bin/env python3
"""Exercise the real OpenGL Nova material upload block and generated sampler.

Requires Windows MSVC, numpy, PyOpenGL and glfw. Creates a hidden GL 4.5
context, compiles the upload block from Renderer3D.cpp into a scratch DLL,
and samples it with TEX_SLOT_IDX from the deployed Slang GLSL. No fake GL
calls or rendered results. All generated files stay in Hermes scratch.
"""
from __future__ import annotations

import ctypes
import os
from pathlib import Path
import re
import subprocess
import sys
import unittest

import glfw
import numpy as np
from OpenGL import GL as gl

ROOT = Path(__file__).resolve().parents[1]
SCRATCH = Path.home() / 'AppData/Local/hermes/cache/scratch/nova_texture_probe'
CPP = ROOT / 'Wasteland/src/Wasteland/Renderer/Renderer3D.cpp'
GLSL = ROOT / 'DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.slang.glsl'


def build_upload_dll():
    source = CPP.read_text(encoding='utf-8')
    start = source.index('            // Populate the GL_TEXTURE_2D_ARRAY')
    end = source.index('            // Ceiling division', start)
    body = source[start:end]
    texture_source = (ROOT / 'Wasteland/src/Platform/OpenGL/OpenGLTexture.cpp').read_text(encoding='utf-8')
    generate = re.search(r'glGenerate(?:Texture)?Mipmap\(m_RendererID\);', texture_source).group()
    wrapper = r'''
#include <glad/glad.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
struct Texture {
    GLuint id; uint32_t w, h;
    GLuint GetRendererID() const { return id; }
    uint32_t GetWidth() const { return w; }
    uint32_t GetHeight() const { return h; }
};
struct Renderer3DData { static constexpr uint32_t MaxTextureSlots = 32; };
struct State {
    std::array<std::shared_ptr<Texture>, 32> TextureSlots;
    GLuint SceneArrayTexture = 0;
    GLuint SceneArrayReadFramebuffer = 0, SceneArrayDrawFramebuffer = 0;
    uint32_t SceneArrayWidth = 0, SceneArrayHeight = 0, TextureSlotIndex = 0;
} s_Data;
extern "C" __declspec(dllexport) int Load(GLADloadproc loader) {
    return gladLoadGLLoader(loader);
}
extern "C" __declspec(dllexport) void Reset() {
    glDeleteTextures(1, &s_Data.SceneArrayTexture);
    glDeleteFramebuffers(1, &s_Data.SceneArrayReadFramebuffer);
    glDeleteFramebuffers(1, &s_Data.SceneArrayDrawFramebuffer);
    s_Data = State{};
    glGenTextures(1, &s_Data.SceneArrayTexture);
    glCreateFramebuffers(1, &s_Data.SceneArrayReadFramebuffer);
    glCreateFramebuffers(1, &s_Data.SceneArrayDrawFramebuffer);
    glBindSampler(15, 0);
}
extern "C" __declspec(dllexport) GLuint Upload(const GLuint *ids, unsigned count) {
    s_Data.TextureSlotIndex = count;
    for (unsigned i = 0; i < count; ++i) {
        if (!ids[i]) { s_Data.TextureSlots[i].reset(); continue; }
        GLint w=0,h=0;
        glGetTextureLevelParameteriv(ids[i],0,GL_TEXTURE_WIDTH,&w);
        glGetTextureLevelParameteriv(ids[i],0,GL_TEXTURE_HEIGHT,&h);
        s_Data.TextureSlots[i] = std::make_shared<Texture>(Texture{ids[i],(uint32_t)w,(uint32_t)h});
    }
''' + body + r'''
    return s_Data.SceneArrayTexture;
}
'''
    wrapper += 'extern "C" __declspec(dllexport) void GenerateMaterialMip(GLuint m_RendererID) {' + generate + '}\n'
    SCRATCH.mkdir(parents=True, exist_ok=True)
    cpp = SCRATCH / 'upload.cpp'
    cpp.write_text(wrapper, encoding='utf-8')
    bat = SCRATCH / 'build.bat'
    glad = ROOT / 'Wasteland/vendor/Glad'
    bat.write_text(
        '@echo off\ncall "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
        f'cl /nologo /std:c++17 /EHsc /LD "{cpp}" "{glad / "src/glad.c"}" '
        f'/I"{glad / "include"}" /Fe:"{SCRATCH / "upload.dll"}" /link opengl32.lib\n'
        'exit /b %ERRORLEVEL%\n', encoding='utf-8')
    result = subprocess.run(['cmd.exe', '/c', str(bat)], cwd=SCRATCH,
                            capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    dll = ctypes.CDLL(str(SCRATCH / 'upload.dll'))
    loader_type = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_char_p)
    loader = loader_type(lambda name: glfw.get_proc_address(name.decode()))
    dll.Load.argtypes = [loader_type]
    assert dll.Load(loader), 'GLAD could not load the live GL context'
    dll.Upload.argtypes = [ctypes.POINTER(ctypes.c_uint), ctypes.c_uint]
    dll.Upload.restype = ctypes.c_uint
    dll.GenerateMaterialMip.argtypes = [ctypes.c_uint]
    return dll, loader


def sampler_program():
    src = GLSL.read_text(encoding='utf-8')
    start = src.index('vec4 TEX_SLOT_IDX_0(')
    opening = src.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    function = src[start:end]
    decl = re.search(r'layout\(binding\s*=\s*15\)\s*uniform sampler2DArray u_SceneTextures_0;', src)
    assert decl, 'Expected generated Nova material sampler at unit 15'
    code = '#version 450\n' + decl.group() + '\n' + function + r'''
layout(local_size_x=1) in;
layout(std430,binding=0) buffer Results { vec4 pixels[]; };
uniform int slot;
uniform float offsetUV;
uniform float sampleLod;
void main() {
    uint x=gl_GlobalInvocationID.x, y=gl_GlobalInvocationID.y;
    vec2 uv=vec2(x,y)/32.0 + vec2(0.5/32.0) + offsetUV;
    pixels[y*32+x]=TEX_SLOT_IDX_0(slot,uv,sampleLod);
}
'''
    shader = gl.glCreateShader(gl.GL_COMPUTE_SHADER)
    gl.glShaderSource(shader, code); gl.glCompileShader(shader)
    assert gl.glGetShaderiv(shader, gl.GL_COMPILE_STATUS), gl.glGetShaderInfoLog(shader)
    program = gl.glCreateProgram(); gl.glAttachShader(program, shader); gl.glLinkProgram(program)
    assert gl.glGetProgramiv(program, gl.GL_LINK_STATUS), gl.glGetProgramInfoLog(program)
    gl.glDeleteShader(shader)
    return program


class MaterialUploadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert glfw.init(), 'GLFW init failed'
        glfw.window_hint(glfw.VISIBLE, glfw.FALSE)
        glfw.window_hint(glfw.CONTEXT_VERSION_MAJOR, 4)
        glfw.window_hint(glfw.CONTEXT_VERSION_MINOR, 5)
        glfw.window_hint(glfw.OPENGL_PROFILE, glfw.OPENGL_CORE_PROFILE)
        cls.window = glfw.create_window(32, 32, 'Nova material tests', None, None)
        assert cls.window, 'GL 4.5 context unavailable'
        glfw.make_context_current(cls.window)
        print('GPU:', gl.glGetString(gl.GL_RENDERER).decode(), flush=True)
        cls.dll, cls.loader = build_upload_dll()
        cls.program = sampler_program()
        cls.buffer = gl.glGenBuffers(1)
        gl.glBindBuffer(gl.GL_SHADER_STORAGE_BUFFER, cls.buffer)
        gl.glBufferData(gl.GL_SHADER_STORAGE_BUFFER, 32*32*4*4, None, gl.GL_DYNAMIC_READ)
        gl.glBindBufferBase(gl.GL_SHADER_STORAGE_BUFFER, 0, cls.buffer)

    @classmethod
    def tearDownClass(cls):
        glfw.destroy_window(cls.window)
        glfw.terminate()

    def setUp(self):
        self.dll.Reset()
        self.textures = []

    def tearDown(self):
        while gl.glGetError() != gl.GL_NO_ERROR:
            pass
        gl.glDeleteTextures(self.textures)

    def texture(self, size, rgb=False, color=None, checker=False, levels=1):
        channels = 3 if rgb else 4
        if checker:
            grid = ((np.indices((size,size)).sum(axis=0)//4) % 2 * 255).astype(np.uint8)
            pixels = np.repeat(grid[:,:,None], channels, axis=2)
            if channels == 4: pixels[:,:,3] = 255
        else:
            pixels = np.empty((size,size,channels), dtype=np.uint8)
            pixels[:] = color if color is not None else [255]*channels
        tex = gl.glGenTextures(1); self.textures.append(tex)
        gl.glBindTexture(gl.GL_TEXTURE_2D, tex)
        gl.glTexStorage2D(gl.GL_TEXTURE_2D,levels,gl.GL_RGB8 if rgb else gl.GL_RGBA8,size,size)
        gl.glPixelStorei(gl.GL_UNPACK_ALIGNMENT,1)
        gl.glTexSubImage2D(gl.GL_TEXTURE_2D,0,0,0,size,size,gl.GL_RGB if rgb else gl.GL_RGBA,gl.GL_UNSIGNED_BYTE,pixels)
        return tex

    def upload(self, ids):
        values=(ctypes.c_uint*len(ids))(*ids)
        result=self.dll.Upload(values,len(ids))
        error=gl.glGetError()
        self.assertEqual(error,gl.GL_NO_ERROR,f'Nova material upload produced GL error {error}')
        return result

    def sample(self, slot, offset=0.0, lod=0.0):
        gl.glUseProgram(self.program)
        gl.glUniform1i(gl.glGetUniformLocation(self.program,'slot'),slot)
        gl.glUniform1f(gl.glGetUniformLocation(self.program,'offsetUV'),offset)
        gl.glUniform1f(gl.glGetUniformLocation(self.program,'sampleLod'),lod)
        gl.glDispatchCompute(32,32,1)
        gl.glMemoryBarrier(gl.GL_BUFFER_UPDATE_BARRIER_BIT)
        gl.glBindBuffer(gl.GL_SHADER_STORAGE_BUFFER,self.buffer)
        data=gl.glGetBufferSubData(gl.GL_SHADER_STORAGE_BUFFER,0,32*32*4*4)
        return np.frombuffer(data,dtype=np.float32).reshape(32,32,4)

    def test_viewport_resize_keeps_material_array_alive(self):
        source=CPP.read_text(encoding='utf-8')
        resize=source[source.index('void Renderer3D::ResizeRayTraceTarget('):]
        resize=resize[:resize.index('static bool IsAABBInFrustum(')]
        self.assertNotIn('glDeleteTextures(1, &s_Data.SceneArrayTexture)',resize,
                         'Viewport resize destroys material storage without recreating it')

    def test_source_texture_mipmap_call_uses_texture_not_target(self):
        tex=self.texture(64,checker=True,levels=7)
        self.dll.GenerateMaterialMip(tex)
        error=gl.glGetError()
        self.assertEqual(error,gl.GL_NO_ERROR,f'Material mip generation produced GL error {error}')
        gl.glBindTexture(gl.GL_TEXTURE_2D,tex)
        result=gl.glGetTexImage(gl.GL_TEXTURE_2D,6,gl.GL_RGBA,gl.GL_UNSIGNED_BYTE)
        self.assertAlmostEqual(float(np.frombuffer(result,dtype=np.uint8)[:3].mean())/255,0.5,delta=0.03)

    def test_rgb_jpeg_format_is_converted_to_rgba(self):
        self.upload([self.texture(1), self.texture(225,rgb=True,color=[210,40,20])])
        np.testing.assert_allclose(self.sample(1)[:,:,:3], np.broadcast_to(np.array([210,40,20])/255,(32,32,3)),atol=1/255)

    def test_material_uvs_tile_instead_of_clamping(self):
        self.upload([self.texture(1),self.texture(64,checker=True)])
        inside=self.sample(1)
        outside=self.sample(1,offset=2.0)
        self.assertGreater(float(outside[:,:,0].std()),0.1,'Tiled material collapsed to a constant edge texel')
        np.testing.assert_allclose(outside,inside,atol=1/255)

    def test_mixed_sizes_fill_the_entire_layer_and_keep_slot_31(self):
        ids=[self.texture(1)]*32
        ids[1]=self.texture(16,color=[20,200,40,255])
        ids[31]=self.texture(64,color=[180,30,220,255])
        self.upload(ids)
        np.testing.assert_allclose(self.sample(1)[:,:,:3], np.broadcast_to(np.array([20,200,40])/255,(32,32,3)),atol=1/255)
        np.testing.assert_allclose(self.sample(31)[:,:,:3], np.broadcast_to(np.array([180,30,220])/255,(32,32,3)),atol=1/255)
        np.testing.assert_allclose(self.sample(0),1,atol=1/255)

    def test_screen_space_sampler_does_not_override_material_tiling(self):
        sampler=int(gl.glGenSamplers(1))
        try:
            gl.glSamplerParameteri(sampler,gl.GL_TEXTURE_WRAP_S,gl.GL_CLAMP_TO_EDGE)
            gl.glSamplerParameteri(sampler,gl.GL_TEXTURE_WRAP_T,gl.GL_CLAMP_TO_EDGE)
            gl.glBindSampler(15,sampler)
            self.upload([self.texture(1),self.texture(64,checker=True)])
            self.assertGreater(float(self.sample(1,offset=2)[:,:,0].std()),0.1)
        finally:
            gl.glBindSampler(15,0)
            gl.glDeleteSamplers(1,[sampler])

    def test_upload_ignores_scissor_but_restores_its_state(self):
        box=gl.glGetIntegerv(gl.GL_SCISSOR_BOX)
        gl.glScissor(1,1,1,1)
        gl.glEnable(gl.GL_SCISSOR_TEST)
        try:
            self.upload([self.texture(1),self.texture(32,color=[10,80,210,255])])
            self.assertTrue(gl.glIsEnabled(gl.GL_SCISSOR_TEST))
            np.testing.assert_array_equal(gl.glGetIntegerv(gl.GL_SCISSOR_BOX),[1,1,1,1])
            np.testing.assert_allclose(self.sample(1)[:,:,:3], np.broadcast_to(np.array([10,80,210])/255,(32,32,3)),atol=1/255)
        finally:
            gl.glDisable(gl.GL_SCISSOR_TEST)
            gl.glScissor(*map(int,box))

    def test_source_content_changes_refresh_existing_layer(self):
        tex=self.texture(32,color=[220,10,20,255])
        ids=[self.texture(1),tex]
        array=self.upload(ids)
        pixels=np.full((32,32,4),[20,200,40,255],dtype=np.uint8)
        gl.glBindTexture(gl.GL_TEXTURE_2D,tex)
        gl.glTexSubImage2D(gl.GL_TEXTURE_2D,0,0,0,32,32,gl.GL_RGBA,gl.GL_UNSIGNED_BYTE,pixels)
        self.assertEqual(self.upload(ids),array)
        np.testing.assert_allclose(self.sample(1)[:,:,:3], np.broadcast_to(np.array([20,200,40])/255,(32,32,3)),atol=1/255)

    def test_growth_recreates_immutable_storage(self):
        self.upload([self.texture(1),self.texture(16)])
        array=self.upload([self.texture(1),self.texture(64,color=[20,60,220,255])])
        gl.glBindTexture(gl.GL_TEXTURE_2D_ARRAY,array)
        self.assertEqual(gl.glGetTexLevelParameteriv(gl.GL_TEXTURE_2D_ARRAY,0,gl.GL_TEXTURE_WIDTH),64)

    def test_material_mip_chain_is_initialized(self):
        array=self.upload([self.texture(1),self.texture(64,checker=True)])
        gl.glBindTexture(gl.GL_TEXTURE_2D_ARRAY,array)
        self.assertEqual(gl.glGetTexParameteriv(gl.GL_TEXTURE_2D_ARRAY,gl.GL_TEXTURE_IMMUTABLE_LEVELS),7)
        np.testing.assert_allclose(self.sample(1,lod=6)[:,:,:3],0.5,atol=0.03)


if __name__ == '__main__':
    unittest.main()
