//========================================================================
// Win32 DXGI fallback test
//========================================================================

#define GLAD_GL_IMPLEMENTATION
#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <stdio.h>
#include <stdlib.h>

static void errorCallback(int error, const char *description) {
    fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

static void keyCallback(GLFWwindow *window, int key, int scancode, int action,
                        int mods) {
    (void)scancode;
    (void)mods;

    if (action == GLFW_PRESS && key == GLFW_KEY_ESCAPE)
        glfwSetWindowShouldClose(window, GLFW_TRUE);
}

static void framebufferSizeCallback(GLFWwindow *window, int width, int height) {
    (void)window;
    (void)width;
    (void)height;

    // Any size change may recreate the DXGI interop texture.
    glfwSetWindowUserPointer(window, (void *)1);
}

static GLuint compileShader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei length = 0;
        glGetShaderInfoLog(shader, (GLsizei)sizeof(log), &length, log);
        fprintf(stderr, "Shader compilation failed: %.*s\n", (int)length, log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static GLuint createHdrProgram(void) {
    static const char *vertexSource =
        "#version 430 core\n"
        "layout(location = 0) in vec2 aPos;\n"
        "layout(location = 0) out vec2 vPos;\n"
        "void main()\n"
        "{\n"
        "    vPos = aPos;\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";
    static const char *fragmentSource =
        "#version 430 core\n"
        "layout(location = 0) in vec2 vPos;\n"
        "layout(location = 0) out vec4 FragColor;\n"
        "void main()\n"
        "{\n"
        "    vec3 color = vec3(0.0);\n"
        "    if (abs(vPos.x) < 0.65 && abs(vPos.y) < 0.35)\n"
        "    {\n"
        "        float u = (vPos.x + 0.65) / 1.3;\n"
        "        float v = (vPos.y + 0.35) / 0.7;\n"
        "        color = vec3(\n"
        "            mix(1.45, 0.05, u),\n"
        "            mix(0.05, 1.35, v),\n"
        "            0.10 + 1.35 * (u * v));\n"
        "    }\n"
        "    FragColor = vec4(color * 50.0, 1.0);\n"
        "}\n";
    GLuint vertexShader = 0;
    GLuint fragmentShader = 0;
    GLuint program = 0;
    GLint ok = 0;

    vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource);
    fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertexShader || !fragmentShader)
        goto fail;

    program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glBindAttribLocation(program, 0, "aPos");
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei length = 0;
        glGetProgramInfoLog(program, (GLsizei)sizeof(log), &length, log);
        fprintf(stderr, "Program link failed: %.*s\n", (int)length, log);
        goto fail;
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    return program;

fail:
    if (program)
        glDeleteProgram(program);
    if (vertexShader)
        glDeleteShader(vertexShader);
    if (fragmentShader)
        glDeleteShader(fragmentShader);
    return 0;
}

static int attachSwapchainTextureToFbo(GLuint fbo, uint32_t texture) {
    GLenum status;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           texture, 0);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(
            stderr,
            "Failed to attach swapchain texture %u to FBO (status=0x%04x)\n",
            texture, status);
        return 0;
    }

    return 1;
}

static void drawHdrWcgRectangle(GLuint program, GLuint vbo, int width,
                                int height) {
    glViewport(0, 0, width, height);
    glClearColor(0.01f, 0.01f, 0.01f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(program);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                          (void *)0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

int main(void) {
    static const float quadVertices[] = {-1.0f, -1.0f, 1.0f,  -1.0f,
                                         1.0f,  1.0f,  -1.0f, -1.0f,
                                         1.0f,  1.0f,  -1.0f, 1.0f};
    GLFWwindow *window;
    uint32_t texture = 0;
    uint64_t imageHandle = 0;
    GLuint program = 0;
    GLuint fbo = 0;
    GLuint vbo = 0;
    int width, height;
    int lastWidth = -1;
    int lastHeight = -1;

    glfwSetErrorCallback(errorCallback);

    if (!glfwInit())
        return EXIT_FAILURE;

    // Ask for a configuration that often fails on legacy WGL paths.
    glfwWindowHint(GLFW_FLOATBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_RED_BITS, 16);
    glfwWindowHint(GLFW_GREEN_BITS, 16);
    glfwWindowHint(GLFW_BLUE_BITS, 16);
    glfwWindowHint(GLFW_ALPHA_BITS, 16);
    glfwWindowHint(GLFW_WIN32_DXGI_SWAPCHAIN_FALLBACK, GLFW_TRUE);
    glfwWindowHint(GLFW_WIN32_DXGI_SWAPCHAIN_FORCE, GLFW_TRUE);

    window = glfwCreateWindow(960, 540, "DXGI fallback probe", NULL, NULL);
    if (!window) {
        const char *description;
        glfwGetError(&description);
        fprintf(stderr, "Failed to create window: %s\n", description);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwSetKeyCallback(window, keyCallback);
    glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);
    glfwSetWindowUserPointer(window, (void *)1);
    glfwMakeContextCurrent(window);
    if (!gladLoadGL(glfwGetProcAddress)) {
        fprintf(stderr, "Failed to load OpenGL entry points\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwSwapInterval(0);

    texture = glfwGetWindowSwapchainImageTexture(window);
    imageHandle = glfwGetWin32SwapchainImageHandle(window);

    printf("DXGI fallback texture id: %u\n", texture);
    printf("DXGI fallback image handle: %llu\n",
           (unsigned long long)imageHandle);

    if (!texture) {
        fprintf(stderr, "DXGI fallback texture is unavailable, cannot render "
                        "to swapchain image texture.\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    if (!imageHandle)
        printf("Warning: native image handle is 0.\n");

    program = createHdrProgram();
    if (!program) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices,
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenFramebuffers(1, &fbo);
    if (!attachSwapchainTextureToFbo(fbo, texture)) {
        glDeleteFramebuffers(1, &fbo);
        glDeleteBuffers(1, &vbo);
        glDeleteProgram(program);
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    printf("Rendering HDR+WCG rectangle via shader into "
           "glfwGetWindowSwapchainImageTexture().\n");

    while (!glfwWindowShouldClose(window)) {
        uint32_t updatedTexture;
        int needReattach = glfwGetWindowUserPointer(window) != NULL;

        glfwPollEvents();

        updatedTexture = glfwGetWindowSwapchainImageTexture(window);
        if (!updatedTexture) {
            fprintf(stderr, "Swapchain image texture became unavailable.\n");
            break;
        }

        if (updatedTexture != texture) {
            texture = updatedTexture;
            needReattach = 1;
        }

        glfwGetFramebufferSize(window, &width, &height);
        if (width <= 0 || height <= 0)
            continue;

        if (width != lastWidth || height != lastHeight) {
            lastWidth = width;
            lastHeight = height;
            needReattach = 1;
        }

        if (needReattach) {
            if (!attachSwapchainTextureToFbo(fbo, texture))
                break;

            glfwSetWindowUserPointer(window, NULL);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        drawHdrWcgRectangle(program, vbo, width, height);
        glfwSwapBuffers(window);
    }

    glDeleteFramebuffers(1, &fbo);
    glDeleteBuffers(1, &vbo);
    glDeleteProgram(program);

    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
