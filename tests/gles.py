"""OpenGL ES 3 sin pantalla (EGL surfaceless + Mesa llvmpipe) para probar los shaders generados.
Si el sistema no tiene EGL/Mesa, `available()` devuelve False y las pruebas se omiten."""
import ctypes as C

GL_VERTEX_SHADER, GL_FRAGMENT_SHADER = 0x8B31, 0x8B30
GL_COMPILE_STATUS, GL_LINK_STATUS = 0x8B81, 0x8B82
GL_TEXTURE_2D, GL_RGBA, GL_UNSIGNED_BYTE, GL_FLOAT = 0x0DE1, 0x1908, 0x1401, 0x1406
GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, GL_RGBA8 = 0x8D40, 0x8CE0, 0x8D41, 0x8058
GL_TRIANGLE_STRIP, GL_COLOR_BUFFER_BIT = 0x0005, 0x4000
GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_NEAREST, GL_TEXTURE0 = 0x2801, 0x2800, 0x2600, 0x84C0

_ctx = None


def available() -> bool:
    global _ctx
    if _ctx is not None:
        return _ctx is not False
    try:
        egl = C.CDLL("libEGL.so.1")
        gl = C.CDLL("libGLESv2.so.2")
        egl.eglGetProcAddress.restype = C.c_void_p
        gpd = C.CFUNCTYPE(C.c_void_p, C.c_uint, C.c_void_p, C.c_void_p)(egl.eglGetProcAddress(b"eglGetPlatformDisplayEXT"))
        dpy = gpd(0x31DD, None, None)
        egl.eglInitialize.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p]
        if not egl.eglInitialize(dpy, None, None):
            raise OSError
        egl.eglBindAPI(0x30A0)
        attrs = (C.c_int * 3)(0x3040, 0x0040, 0x3038)
        cfg, n = C.c_void_p(), C.c_int()
        egl.eglChooseConfig.argtypes = [C.c_void_p, C.c_void_p, C.POINTER(C.c_void_p), C.c_int, C.POINTER(C.c_int)]
        egl.eglChooseConfig(dpy, attrs, C.byref(cfg), 1, C.byref(n))
        egl.eglCreateContext.restype = C.c_void_p
        egl.eglCreateContext.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]
        ctx = egl.eglCreateContext(dpy, cfg, None, (C.c_int * 3)(0x3098, 3, 0x3038))
        egl.eglMakeCurrent.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]
        if not ctx or not egl.eglMakeCurrent(dpy, None, None, ctx):
            raise OSError
        gl.glGetString.restype = C.c_char_p
        gl.glGetUniformLocation.restype = C.c_int
        _ctx = gl
        _setup_fbo(gl)
        return True
    except (OSError, AttributeError):
        _ctx = False
        return False


def _setup_fbo(gl, size=4):
    fb, rb = C.c_uint(), C.c_uint()
    gl.glGenFramebuffers(1, C.byref(fb))
    gl.glBindFramebuffer(GL_FRAMEBUFFER, fb)
    gl.glGenRenderbuffers(1, C.byref(rb))
    gl.glBindRenderbuffer(GL_RENDERBUFFER, rb)
    gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, size, size)
    gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb)
    gl.glViewport(0, 0, size, size)


def renderer() -> str:
    return _ctx.glGetString(0x1F01).decode() if available() else ""


def _shader(kind, src):
    gl = _ctx
    s = gl.glCreateShader(kind)
    b = C.c_char_p(src.encode())
    gl.glShaderSource(s, 1, C.byref(b), None)
    gl.glCompileShader(s)
    ok = C.c_int()
    gl.glGetShaderiv(s, GL_COMPILE_STATUS, C.byref(ok))
    if not ok.value:
        log = C.create_string_buffer(4096)
        gl.glGetShaderInfoLog(s, 4096, None, log)
        raise RuntimeError(log.value.decode(errors="replace"))
    return s


def program(vert: str, frag: str) -> int:
    gl = _ctx
    p = gl.glCreateProgram()
    gl.glAttachShader(p, _shader(GL_VERTEX_SHADER, vert))
    gl.glAttachShader(p, _shader(GL_FRAGMENT_SHADER, frag))
    gl.glLinkProgram(p)
    ok = C.c_int()
    gl.glGetProgramiv(p, GL_LINK_STATUS, C.byref(ok))
    if not ok.value:
        log = C.create_string_buffer(4096)
        gl.glGetProgramInfoLog(p, 4096, None, log)
        raise RuntimeError(log.value.decode(errors="replace"))
    return p


def draw(prog: int, textures: dict[int, tuple], vertex_color=(255, 255, 255, 255), clear=(0, 0, 0, 0)):
    """Dibuja un cuadro a pantalla completa (normal hacia la luz) y devuelve el píxel central RGBA."""
    gl = _ctx
    gl.glUseProgram(prog)
    ident = (C.c_float * 16)(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
    for name in (b"mvp", b"mv"):
        gl.glUniformMatrix4fv(gl.glGetUniformLocation(prog, name), 1, 0, ident)
    gl.glUniform3f(gl.glGetUniformLocation(prog, b"light"), C.c_float(0), C.c_float(0), C.c_float(1))
    for unit, rgba in textures.items():
        t = C.c_uint()
        gl.glGenTextures(1, C.byref(t))
        gl.glActiveTexture(GL_TEXTURE0 + unit)
        gl.glBindTexture(GL_TEXTURE_2D, t)
        gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, (C.c_ubyte * 4)(*rgba))
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST)
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST)
        gl.glUniform1i(gl.glGetUniformLocation(prog, f"tex{unit}".encode()), unit)
    pos = (C.c_float * 12)(-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0)
    nrm = (C.c_float * 12)(*([0, 0, 1] * 4))
    col = (C.c_ubyte * 16)(*(list(vertex_color) * 4))
    uv = (C.c_float * 8)(0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5)
    gl.glBindVertexArray(0)
    for loc, n, typ, norm, arr in ((0, 3, GL_FLOAT, 0, pos), (1, 3, GL_FLOAT, 0, nrm), (2, 4, GL_UNSIGNED_BYTE, 1, col),
                                   (3, 2, GL_FLOAT, 0, uv)):
        gl.glVertexAttribPointer(loc, n, typ, norm, 0, arr)
        gl.glEnableVertexAttribArray(loc)
    gl.glClearColor(*(C.c_float(c / 255) for c in clear))
    gl.glClear(GL_COLOR_BUFFER_BIT)
    gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)
    px = (C.c_ubyte * 4)()
    gl.glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px)
    return tuple(px)
