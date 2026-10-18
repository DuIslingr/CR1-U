#pragma once
// Minimal GL function table shared by desktop GL 3.3 core and GLES 3.0.
// Functions live in namespace gl (gl::Clear, gl::DrawArrays, ...) so they never
// clash with whatever prototypes the platform's GL headers declare.

#ifdef CR_GLES
#include <GLES3/gl3.h>
#else
#include <SDL3/SDL_opengl.h>
#endif
#ifndef APIENTRY
#define APIENTRY // (GLES's headers call it GL_APIENTRY)
#endif

// X(return type, name, parameter list)
#define CR_GL_FUNCTIONS(X) \
    X(void, Viewport, (GLint x, GLint y, GLsizei w, GLsizei h)) \
    X(void, ClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
    X(void, Clear, (GLbitfield mask)) \
    X(void, Enable, (GLenum cap)) \
    X(void, Disable, (GLenum cap)) \
    X(void, BlendFunc, (GLenum s, GLenum d)) \
    X(GLenum, GetError, (void)) \
    X(const GLubyte*, GetString, (GLenum name)) \
    X(void, PixelStorei, (GLenum pname, GLint param)) \
    X(void, GenTextures, (GLsizei n, GLuint* t)) \
    X(void, DeleteTextures, (GLsizei n, const GLuint* t)) \
    X(void, BindTexture, (GLenum target, GLuint t)) \
    X(void, ActiveTexture, (GLenum unit)) \
    X(void, TexImage2D, (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, \
                         GLenum fmt, GLenum type, const void* data)) \
    X(void, TexParameteri, (GLenum target, GLenum pname, GLint param)) \
    X(void, GenerateMipmap, (GLenum target)) \
    X(GLuint, CreateShader, (GLenum type)) \
    X(void, ShaderSource, (GLuint s, GLsizei n, const GLchar* const* src, const GLint* len)) \
    X(void, CompileShader, (GLuint s)) \
    X(void, GetShaderiv, (GLuint s, GLenum pname, GLint* out)) \
    X(void, GetShaderInfoLog, (GLuint s, GLsizei max, GLsizei* len, GLchar* log)) \
    X(void, DeleteShader, (GLuint s)) \
    X(GLuint, CreateProgram, (void)) \
    X(void, AttachShader, (GLuint p, GLuint s)) \
    X(void, BindAttribLocation, (GLuint p, GLuint index, const GLchar* name)) \
    X(void, LinkProgram, (GLuint p)) \
    X(void, GetProgramiv, (GLuint p, GLenum pname, GLint* out)) \
    X(void, GetProgramInfoLog, (GLuint p, GLsizei max, GLsizei* len, GLchar* log)) \
    X(void, DeleteProgram, (GLuint p)) \
    X(void, UseProgram, (GLuint p)) \
    X(GLint, GetUniformLocation, (GLuint p, const GLchar* name)) \
    X(void, Uniform1i, (GLint loc, GLint v)) \
    X(void, Uniform4f, (GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w)) \
    X(void, UniformMatrix4fv, (GLint loc, GLsizei n, GLboolean transpose, const GLfloat* m)) \
    X(void, GenVertexArrays, (GLsizei n, GLuint* a)) \
    X(void, DeleteVertexArrays, (GLsizei n, const GLuint* a)) \
    X(void, BindVertexArray, (GLuint a)) \
    X(void, GenBuffers, (GLsizei n, GLuint* b)) \
    X(void, DeleteBuffers, (GLsizei n, const GLuint* b)) \
    X(void, BindBuffer, (GLenum target, GLuint b)) \
    X(void, BufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage)) \
    X(void, BufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data)) \
    X(void, VertexAttribPointer, (GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, \
                                  const void* ptr)) \
    X(void, EnableVertexAttribArray, (GLuint i)) \
    X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count)) \
    X(void, ReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void* data)) \
    X(void, DrawElements, (GLenum mode, GLsizei count, GLenum type, const void* indices)) \
    X(void, DepthFunc, (GLenum func)) \
    X(void, DepthMask, (GLboolean flag)) \
    X(void, ColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a)) \
    X(void, CullFace, (GLenum mode)) \
    X(void, FrontFace, (GLenum mode)) \
    X(void, Uniform1f, (GLint loc, GLfloat v)) \
    X(void, Uniform3f, (GLint loc, GLfloat x, GLfloat y, GLfloat z))

namespace gl {

#define CR_GL_DECLARE(ret, name, args) extern ret(APIENTRY* name) args;
CR_GL_FUNCTIONS(CR_GL_DECLARE)
#undef CR_GL_DECLARE

// Resolves all functions for the current context. Returns false (and logs) if any are missing.
bool load();

// GLSL header for the current backend: "#version 330 core" or "#version 300 es" + precision.
const char* shaderHeader();

} // namespace gl
