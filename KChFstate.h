#ifndef __MM_KCFSTATE
#define __MM_KCFSTATE
// Привязки-комбинации: две клавиши удерживаются вместе, но с паузой между ними.
// Меньше 0xFF00, чтобы не совпало с кодами кнопок мыши.
#define MM_KEY_LASTEPOCH 0xF100  // Q + пробел + E + R
#define MM_KEY_SONIC 0xF101  // вниз + C
// Пауза между первой и второй клавишей комбинации
#define MM_LASTEPOCH_DELAY_MS 50  // пробел ждёт, пока игра его увидит
#define MM_SONIC_DELAY_MS 100  // Sonic: заметная пауза, чтобы игра видела вниз раньше C
// Конечный автомат для ловли коротких звуков (типа "к" и "ч") [18-DEC]
class KChFstate {
 public:
  enum Direction {
      UP,
      DOWN,
  };
  static void NewFrame(int energy_level);
  static bool IsKCValid();  // может ли к или Ч быть рассмотрена кандидатом на
                            // нажатие (в нужное ли время ?)
  static volatile bool flag_kc_anytime;  // флаг деградирует конечный автомат
                                         // для обработки K/Ch в любое время
  static volatile int next_kc_counter;  // Через сколько фреймов можно нажать
                                        // К/Ч (в упрощенном режиме)
  // [23-AUG-2017] Лучшего места для хранения нажимаемых клавиш не нашлось...
  static volatile WORD key_to_press[6];
  static volatile char repeat_key[6];
  static volatile char toggle_key[6];
  static void SetKeyToPress(int i, WORD key) {
    if ((i >= 0) && (i < 6)) key_to_press[i] = key;
  }
  static WORD GetKeyToPress(int i) {
    if ((i >= 0) && (i < 6)) {
      if (i == 0 && (GetKeyState(VK_CONTROL) & 0x8000)) {
        return 0xFF00;
      }
      return key_to_press[i];
    }
    return 0xffff;
  }
  static char GetRepeatKey(int i) {
    if ((i >= 0) && (i < 6)) {
      if (i == 0 && (GetKeyState(VK_CONTROL) & 0x8000))
      {
        return (char) 1;
      }
      return repeat_key[i];
    }
    return 0;
  }
  static void SetRepeatKey(int i, char repeat) {
    if ((i >= 0) && (i < 6)) repeat_key[i] = repeat;
  }
  static void SetToggleKey(int i, char toggle) {
    if ((i >= 0) && (i < 6)) toggle_key[i] = toggle;
  }
  static char GetToggleKey(int i) {
    if ((i >= 0) && (i < 6)) {
      char toggle = toggle_key[i];
      if (i == 0 && (GetKeyState(VK_CONTROL) & 0x8000)) {
        return (char) 0;
      }
      return toggle_key[i];
    }
    return 0;
  }
  static LONG TryToPress(int i, LONG move);
  static bool IsKeyDown(int i) {
    if ((i >= 0) && (i < 6)) return key_state[i] != 0;
    return false;
  }
 protected:
  static volatile int state, minlevel, counter;
  static int key_state[6];  // нажата-отжата
  static int key_cycle_counter[6];  // Сколько ещё циклов нельзя менять
                                    // состояние клавиши
  static void MM_KeyDown(
      int i);  // Вспомогательная функция для нажатия на клавишу
  static void MM_KeyUp(
      int i);  // Вспомогательная функция для отпускания клавишу
  static void MM_Key(int i, Direction dir);
};
#endif