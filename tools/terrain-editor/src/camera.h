#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>

struct Camera {
    glm::vec3 pos   = {256.f, 50.f, 256.f};
    float     yaw   = 0.f;   // degrees, 0 = looking +Z
    float     pitch = 25.f;  // degrees, positive = looking down
    float     speed = 40.f;
    float     sens  = 0.12f;

    glm::vec3 Forward() const {
        float y = glm::radians(yaw);
        float p = glm::radians(pitch);
        return glm::normalize(glm::vec3(
            std::sin(y) * std::cos(p),
           -std::sin(p),
            std::cos(y) * std::cos(p)
        ));
    }

    glm::vec3 Right() const {
        return glm::normalize(glm::cross(Forward(), glm::vec3(0.f, 1.f, 0.f)));
    }

    glm::mat4 View() const {
        return glm::lookAt(pos, pos + Forward(), glm::vec3(0.f, 1.f, 0.f));
    }

    void Move(GLFWwindow* w, float dt) {
        glm::vec3 f = Forward();
        glm::vec3 r = Right();
        glm::vec3 u = glm::vec3(0.f, 1.f, 0.f);
        if (glfwGetKey(w, GLFW_KEY_W) == GLFW_PRESS) pos += f * speed * dt;
        if (glfwGetKey(w, GLFW_KEY_S) == GLFW_PRESS) pos -= f * speed * dt;
        if (glfwGetKey(w, GLFW_KEY_A) == GLFW_PRESS) pos -= r * speed * dt;
        if (glfwGetKey(w, GLFW_KEY_D) == GLFW_PRESS) pos += r * speed * dt;
        if (glfwGetKey(w, GLFW_KEY_E) == GLFW_PRESS) pos += u * speed * dt;
        if (glfwGetKey(w, GLFW_KEY_Q) == GLFW_PRESS) pos -= u * speed * dt;
    }

    void ApplyMouseDelta(float dx, float dy) {
        yaw   += dx * sens;
        pitch += dy * sens;
        pitch  = std::clamp(pitch, -89.f, 89.f);
    }
};
