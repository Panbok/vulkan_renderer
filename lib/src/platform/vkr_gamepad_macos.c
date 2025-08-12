// Intentionally minimal, no includes to avoid unused warnings in stub

// Opaque forward declaration to avoid depending on full header here
typedef struct Gamepad Gamepad;
typedef struct InputState InputState;

#if defined(PLATFORM_APPLE)

bool8_t vkr_gamepad_init(VkrGamepad *gamepad, int32_t id) {
  (void)gamepad;
  (void)id;
  return true_v;
}

bool8_t vkr_gamepad_connect(VkrGamepad *gamepad) {
  (void)gamepad;
  return false_v;
}

bool8_t vkr_gamepad_poll(VkrGamepad *gamepad, InputState *input_state) {
  (void)gamepad;
  (void)input_state;
  return true_v;
}

bool8_t vkr_gamepad_disconnect(VkrGamepad *gamepad) {
  (void)gamepad;
  return true_v;
}

bool8_t vkr_gamepad_shutdown(VkrGamepad *gamepad) {
  (void)gamepad;
  return true_v;
}

#endif // PLATFORM_APPLE
