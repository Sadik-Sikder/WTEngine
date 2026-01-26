#include <GLFW/glfw3.h>
#include "Engine.h"
#include "OpenGLRenderer.h"

int main() {
    if (!glfwInit()) return -1;

    GLFWwindow* window = glfwCreateWindow(900, 600, "ToyEngine OpenGL", nullptr, nullptr);
    if (!window) { glfwTerminate(); return -1; }

    glfwMakeContextCurrent(window);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Engine engine(900, 600);
    int width = 900, height = 600;

    OpenGLRenderer  renderer;

    engine.loadHTML(L"<html><body><div style='background:#cfe8ff;padding:8px'>Hello OpenGL!</div></body></html>");

    while (!glfwWindowShouldClose(window)) {
        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);

        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        engine.onResize(width, height); 
        renderer.beginFrame(width, height, 0);
        engine.render(renderer);       

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwTerminate();
    return 0;
}
