#pragma once

class Renderer {
public:
    ~Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    static bool Init();
    static void Destroy();
    static void Thread();

    static bool IsOpen();
    static bool IsCompatibilityMode();
    static bool SupportsStreamproof();
    static void SetVSync(bool enable);
private:
    Renderer() {};

    static Renderer& GetInstance()
    {
        static Renderer i{};
        return i;
    }

    bool InitImpl();
    void ThreadImpl();
    void DestroyImpl();

    void Render();
    bool HandleState();
    bool HandleWindowOrder();
private:
    bool isRunning = true;
    bool isOpen = false;
    bool compatibilityMode = false;

    bool isFocused = false;
};
