#include "Gl.h"

#include <SDL.h>

#include <bit>
#include <cstdio>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

namespace gl {

#define CHESS_GL_DEFINE(type, name, ...) type (*name)(__VA_ARGS__) = nullptr;
CHESS_GL_FUNCTIONS(CHESS_GL_DEFINE)
#undef CHESS_GL_DEFINE

namespace {

void LoadFunctions()
{
    std::vector<std::string> missing;
#define CHESS_GL_LOAD(type, name, ...)                                                                                                                       \
    name = std::bit_cast<decltype(name)>(SDL_GL_GetProcAddress(#name));                                                                                        \
    if (name == nullptr)                                                                                                                                       \
        missing.emplace_back(#name);
    CHESS_GL_FUNCTIONS(CHESS_GL_LOAD)
#undef CHESS_GL_LOAD
    if (!missing.empty())
        throw std::runtime_error(std::format("The graphics driver lacks {} OpenGL functions, such as {}.", missing.size(), missing.front()));
}

#ifndef NDEBUG
void DebugMessage(GLenum, GLenum, GLuint, GLenum severity, GLsizei, const GLchar* message, const void*)
{
    if (severity == GL_DEBUG_SEVERITY_HIGH || severity == GL_DEBUG_SEVERITY_MEDIUM)
        std::fprintf(stderr, "OpenGL: %s\n", message);
}
#endif

} // namespace

Buffer CreateBuffer(const void* data, std::size_t size, GLbitfield flags)
{
    Buffer buffer = CreateBuffer();
    glNamedBufferStorage(buffer.Id(), static_cast<GLsizeiptr>(size), data, flags);
    return buffer;
}

Buffer CreateBuffer()
{
    GLuint id = 0;
    glCreateBuffers(1, &id);
    return Buffer(id);
}

VertexArray CreateVertexArray()
{
    GLuint id = 0;
    glCreateVertexArrays(1, &id);
    return VertexArray(id);
}

Texture CreateTexture(GLenum target)
{
    GLuint id = 0;
    glCreateTextures(target, 1, &id);
    return Texture(id);
}

Sampler CreateSampler()
{
    GLuint id = 0;
    glCreateSamplers(1, &id);
    return Sampler(id);
}

Framebuffer CreateFramebuffer()
{
    GLuint id = 0;
    glCreateFramebuffers(1, &id);
    return Framebuffer(id);
}

void Context::RequestPixelFormat()
{
    // The scene is drawn into framebuffers of its own; the window keeps a depth buffer for the pieces of the promotion
    // choice, drawn straight into it.
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifndef NDEBUG
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#endif
}

Context::Context(SDL_Window* window) : m_window(window)
{
    // OpenGL 4.6 core, or 4.5 where the driver stops there.
    for (const int minor : {6, 5}) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
        m_context = SDL_GL_CreateContext(window);
        if (m_context)
            break;
    }
    if (!m_context)
        throw std::runtime_error(std::format("OpenGL 4.5 or newer is not available: {}\nPlease update the graphics driver.", SDL_GetError()));

    try {
        LoadFunctions();
    }
    catch (...) {
        SDL_GL_DeleteContext(m_context);
        throw;
    }

    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    m_version = major * 10 + minor;

    SDL_GL_SetSwapInterval(1);

#ifndef NDEBUG
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(DebugMessage, nullptr);
#endif
}

Context::~Context()
{
    SDL_GL_MakeCurrent(m_window, nullptr);
    SDL_GL_DeleteContext(m_context);
}

void Context::SwapBuffers() const { SDL_GL_SwapWindow(m_window); }

} // namespace gl
