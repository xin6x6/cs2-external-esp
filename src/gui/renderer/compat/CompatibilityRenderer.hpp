#pragma once

class CompatibilityRenderer {
public:
	static bool Init();
	static void Destroy();
	static void Thread();

	static bool IsOpen();
	static void SetOpen(bool open);
	static void SetVSync(bool enable);
};
