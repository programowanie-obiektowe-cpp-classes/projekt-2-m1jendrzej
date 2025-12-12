#include <stdint.h>        // typy całkowite o stałej szerokości (int16_t, uint8_t)
#include <Arduino.h>       // Serial.begin, Serial.println, pinMode, digitalWrite, digitalRead, millis, delay, random, randomSeed, esp_random, ledcAttachChannel, ledcWriteTone
#include <Adafruit_GFX.h>  // bazowa klasa graficzna dla Adafruit_SSD1306 (pośrednio: drawPixel)
#include <Adafruit_SSD1306.h> // Adafruit_SSD1306: konstruktor, clearDisplay, display, drawPixel

#include <vector>          // std::vector (kontenery na pociski i wrogów)
#include <variant>         // std::variant, std::visit, std::get_if, std::holds_alternative
#include <memory>          // std::unique_ptr, std::make_unique
#include <array>           // std::array (stałe tablice np. pinów, pozycji spawnu)
#include <algorithm>       // std:any_of, std::swap
#include <numeric>         // std::iota (do permutacji kolejności spawnu)
#include <cctype>          // std::toupper (konwersja znaku do wielkiej litery)
#include <cstdio>          // std::snprintf (formatowanie napisu z wynikiem)
#include <cstring>         // std::strlen (długość tekstu)
#include <exception>       // std::exception 
#include <limits>          // std::numeric_limits (górny limit dla typu wyniku)

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool BuzzerSounds = false;

// ============================================================
//                      Exceptions
// ============================================================

// Klasa wyjątku sygnalizująca błąd inicjalizacji wyświetlacza SSD1306.
class DisplayInitError : public std::exception {
public:
  // Zwraca komunikat opisujący przyczynę błędu inicjalizacji wyświetlacza.
  const char* what() const noexcept override {
    return "Display initialization failed (SSD1306).";
  }
};

// Klasa wyjątku sygnalizująca próbę utworzenia obiektu gry poza obszarem ekranu.
class OffscreenObjectError : public std::exception {
public:
  // Zwraca komunikat informujący o próbie utworzenia obiektu poza ekranem.
  const char* what() const noexcept override {
    return "Attempt to create game object outside of screen area.";
  }
};

// Klasa wyjątku sygnalizująca ustawienie liczby HP poza zakresem możliwym do
// odwzorowania na diodach LED (więcej niż liczba diod lub wartość ujemna).
class HpLedRangeError : public std::exception {
public:
  // Zwraca komunikat o błędnym zakresie HP względem liczby diod LED.
  const char* what() const noexcept override {
    return "HpAndShoots: HP value out of LED range.";
  }
};

// Klasa wyjątku sygnalizująca przekroczenie maksymalnej liczby wrogów
// obsługiwanych przez silnik gry (przepełnienie wektora enemies).
class EnemyOverflowError : public std::exception {
public:
  // Zwraca komunikat o przekroczeniu limitu liczby wrogów.
  const char* what() const noexcept override {
    return "GameEngine: enemy container overflow (MAX_ENEMIES reached).";
  }
};

// Klasa wyjątku sygnalizująca przekroczenie maksymalnej możliwej wartości wyniku.
class ScoreOverflowError : public std::exception {
public:
  // Zwraca komunikat o zbyt wysokim (przepełnionym) wyniku gracza.
  const char* what() const noexcept override {
    return "GameEngine: score overflow (too high).";
  }
};

// ============================================================
//                      Screen
// ============================================================

class Screen {
public:
  // Konstruktor: przechowuje referencję do obiektu wyświetlacza OLED,
  // aby móc po nim rysować na podstawie bufora board.
  explicit Screen(Adafruit_SSD1306& disp) : display(disp) {}

  // Czyści dwuwymiarową tablicę board ustawiając każdy piksel na 0 (tło).
  void clearBoard(uint8_t board[SCREEN_HEIGHT][SCREEN_WIDTH]) const {
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
      for (int x = 0; x < SCREEN_WIDTH; x++) board[y][x] = 0;
    }
  }

  // Ustawia wskazany piksel na planszy board na daną wartość (domyślnie 1),
  // pod warunkiem, że współrzędne mieszczą się w granicach ekranu.
  void setPixelOnBoard(uint8_t board[SCREEN_HEIGHT][SCREEN_WIDTH],
                       int16_t x, int16_t y, uint8_t value = 1) const {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return;
    board[y][x] = value;
  }

  // Rysuje jeden znak 3x5 (odwołując się do bitmapy glyph3x5)
  // w buforze board w pozycji (x,y) za pomocą wartości value.
  void drawChar3x5(uint8_t board[SCREEN_HEIGHT][SCREEN_WIDTH],
                   int16_t x, int16_t y, char c, uint8_t value = 1) const {
    const uint8_t* g = glyph3x5(c);
    if (!g) return;

    for (int row = 0; row < 5; row++) {
      uint8_t bits = g[row];
      for (int col = 0; col < 3; col++) {
        if (bits & (1u << (2 - col))) {
          setPixelOnBoard(board, x + col, y + row, value);
        }
      }
    }
  }

  // Rysuje podany tekst (ciąg znaków) czcionką 3x5 w buforze board,
  // centrowany względem ekranu w poziomie i pionie.
  void drawText3x5Centered(uint8_t board[SCREEN_HEIGHT][SCREEN_WIDTH],
                           const char* text, uint8_t value = 1) const {
    if (!text) return;

    const int charW = 3;
    const int charH = 5;
    const int space = 1;
    const int step  = charW + space;

    const int len = (int)strlen(text);
    if (len <= 0) return;

    int totalW = len * step - space;
    if (totalW < 0) totalW = 0;

    const int16_t startX = (SCREEN_WIDTH  - totalW) / 2;
    const int16_t startY = (SCREEN_HEIGHT - charH) / 2;

    int16_t cx = startX;
    for (int i = 0; i < len; i++) {
      drawChar3x5(board, cx, startY, text[i], value);
      cx += step;
    }
  }

  // Renderuje zawartość tablicy board na fizyczny wyświetlacz.
  // Jeśli rotate180 == true, obraz jest obracany o 180 stopni.
  void render(const uint8_t board[SCREEN_HEIGHT][SCREEN_WIDTH], bool rotate180 = false) {
    display.clearDisplay();

    for (int16_t y = 0; y < SCREEN_HEIGHT; y++) {
      for (int16_t x = 0; x < SCREEN_WIDTH; x++) {
        if (board[y][x] == 0) continue;

        int16_t drawX = x;
        int16_t drawY = y;

        if (rotate180) {
          drawX = SCREEN_WIDTH  - 1 - x;
          drawY = SCREEN_HEIGHT - 1 - y;
        }

        display.drawPixel(drawX, drawY, SSD1306_WHITE);
      }
    }
    display.display();
  }

private:
  // Zwraca wskaźnik na bitmapę 3x5 dla podanego znaku (cyfry, litery S,C,O,R,E lub spacji/ dwukropka).
  // Wykorzystuje std::toupper, aby niezależnie od wielkości liter użyć jednej wersji bitmapy.
  static const uint8_t* glyph3x5(char c) {
    c = (char)std::toupper((unsigned char)c);

    static const uint8_t SP[5] = {0,0,0,0,0};
    static const uint8_t COL[5]= {0,2,0,2,0}; // ':'
    static const uint8_t O0[5] = {7,5,5,5,7};
    static const uint8_t O1[5] = {2,6,2,2,7};
    static const uint8_t O2[5] = {7,1,7,4,7};
    static const uint8_t O3[5] = {7,1,7,1,7};
    static const uint8_t O4[5] = {5,5,7,1,1};
    static const uint8_t O5[5] = {7,4,7,1,7};
    static const uint8_t O6[5] = {7,4,7,5,7};
    static const uint8_t O7[5] = {7,1,1,1,1};
    static const uint8_t O8[5] = {7,5,7,5,7};
    static const uint8_t O9[5] = {7,5,7,1,7};

    static const uint8_t S_[5] = {7,4,7,1,7};
    static const uint8_t C_[5] = {7,4,4,4,7};
    static const uint8_t O_[5] = {7,5,5,5,7};
    static const uint8_t R_[5] = {6,5,6,5,5};
    static const uint8_t E_[5] = {7,4,7,4,7};

    switch (c) {
      case ' ': return SP;
      case ':': return COL;
      case '0': return O0;
      case '1': return O1;
      case '2': return O2;
      case '3': return O3;
      case '4': return O4;
      case '5': return O5;
      case '6': return O6;
      case '7': return O7;
      case '8': return O8;
      case '9': return O9;
      case 'S': return S_;
      case 'C': return C_;
      case 'O': return O_;
      case 'R': return R_;
      case 'E': return E_;
      default:  return SP;
    }
  }

  Adafruit_SSD1306& display;
};

// ============================================================
//                      Pins (buttons)
// ============================================================

class Pins {
public:
  // Konstruktor zapisuje numery pinów do poruszania się i strzałów
  // (lewo/prawo + dwa przyciski strzału).
  Pins(uint8_t leftMovePin, uint8_t rightMovePin, uint8_t shootLeftPin, uint8_t shootRightPin)
  : leftMovePin(leftMovePin), rightMovePin(rightMovePin),
    shootLeftPin(shootLeftPin), shootRightPin(shootRightPin) {}

  // Konfiguruje piny przycisków jako wejścia cyfrowe.
  // Nazwa zmieniona z begin() na configureInputs(), aby nie mylić z begin() kontenerów STL.
  void configureInputs() const {
    pinMode(leftMovePin, INPUT);
    pinMode(rightMovePin, INPUT);
    pinMode(shootLeftPin, INPUT);
    pinMode(shootRightPin, INPUT);
  }

  // Zwraca true jeśli przycisk "ruch w lewo" jest wciśnięty (stan wysoki).
  bool left() const  { 
    return digitalRead(leftMovePin)  == HIGH; 
  }

  // Zwraca true jeśli przycisk "ruch w prawo" jest wciśnięty (stan wysoki).
  bool right() const { 
    return digitalRead(rightMovePin) == HIGH; 
  }

  // Zwraca true jeśli przycisk strzału z lewej strony jest wciśnięty.
  bool shootLeft() const  { 
    return digitalRead(shootLeftPin)  == HIGH; 
  }  // GPIO33

  // Zwraca true jeśli przycisk strzału z prawej strony jest wciśnięty.
  bool shootRight() const { 
    return digitalRead(shootRightPin) == HIGH; 
  }  // GPIO27

private:
  uint8_t leftMovePin;
  uint8_t rightMovePin;
  uint8_t shootLeftPin;
  uint8_t shootRightPin;
};

// ============================================================
//             HpAndShoots (LED HP + buzzer shoot)
// ============================================================

class HpAndShoots {
public:
  // Inicjalizuje piny LED-ów jako wyjścia (gasi wszystkie),
  // konfiguruje kanały PWM/buzzera i ustawia brak dźwięku.
  // Nazwa zmieniona z begin() na initializeOutputs().
  void initializeOutputs() {
    for (uint8_t p : ledPins) {
      pinMode(p, OUTPUT);
      digitalWrite(p, LOW);
    }

    ledcAttachChannel(buzL, DEFAULT_FREQ_HZ, RES_BITS, LEFT_CH);
    ledcAttachChannel(buzR, DEFAULT_FREQ_HZ, RES_BITS, RIGHT_CH);

    ledcWriteTone(buzL, 0);
    ledcWriteTone(buzR, 0);

    leftUntil  = 0;
    rightUntil = 0;
  }

  // Ustawia stan LED-ów HP na podstawie przekazanego hp:
  // - rzuca wyjątek HpLedRangeError jeśli hp jest poza zakresem,
  // - poprawia hp do zakresu [0, liczba diod],
  // - zapala kolejne diody od początku do hp-1.
  void setHpLeds(int hp) {
    try {
      if (hp < 0 || hp > (int)ledPins.size()) {
        throw HpLedRangeError{};
      }
    } catch (const HpLedRangeError& e) {
      Serial.println(e.what());
      // naprawiamy wartość, żeby mimo błędu diody działały sensownie
      if (hp < 0) hp = 0;
      if (hp > (int)ledPins.size()) hp = (int)ledPins.size();
    }

    for (int i = 0; i < (int)ledPins.size(); i++) {
      digitalWrite(ledPins[i], (i < hp) ? HIGH : LOW);
    }
  }

  // Uruchamia dźwięk strzału z lewej strony na określony czas (BUZZ_MS),
  // ustawiając częstotliwość na SHOOT_FREQ_LEFT_HZ.
  void playShootLeft() {
    if (!BuzzerSounds) {
    // dźwięk globalnie wyłączony
      return;
  };

    ledcWriteTone(buzL, SHOOT_FREQ_LEFT_HZ);
    leftUntil = millis() + BUZZ_MS;
  }

  // Uruchamia dźwięk strzału z prawej strony na określony czas (BUZZ_MS),
  // ustawiając częstotliwość na SHOOT_FREQ_RIGHT_HZ.
  void playShootRight() {
      if (!BuzzerSounds) {
    // dźwięk globalnie wyłączony
      return;
    };

    ledcWriteTone(buzR, SHOOT_FREQ_RIGHT_HZ);
    rightUntil = millis() + BUZZ_MS;
  }

  // Natychmiast wyłącza oba buzzery i kasuje czasy zakończenia dźwięku.
  void stopAll() {
    ledcWriteTone(buzL, 0);
    ledcWriteTone(buzR, 0);
    leftUntil = 0;
    rightUntil = 0;
  }

  // Aktualizuje stan buzzerów: jeśli upłynął czas trwania dźwięku,
  // wyłącza odpowiedni kanał.
  void update() {
    const unsigned long now = millis();
    if (leftUntil != 0 && now >= leftUntil) {
      ledcWriteTone(buzL, 0);
      leftUntil = 0;
    }
    if (rightUntil != 0 && now >= rightUntil) {
      ledcWriteTone(buzR, 0);
      rightUntil = 0;
    }
  }

private:
  static constexpr std::array<uint8_t, 4> ledPins = {18, 15, 14, 23};

  static constexpr uint8_t buzL = 26;
  static constexpr uint8_t buzR = 25;

  static constexpr uint32_t DEFAULT_FREQ_HZ = 2000;
  static constexpr uint8_t  RES_BITS = 8;
  static constexpr int8_t   LEFT_CH  = 0;
  static constexpr int8_t   RIGHT_CH = 1;

  static constexpr uint32_t SHOOT_FREQ_LEFT_HZ  = 1600;
  static constexpr uint32_t SHOOT_FREQ_RIGHT_HZ = 1000;
  static constexpr unsigned long BUZZ_MS = 300;

  unsigned long leftUntil = 0;
  unsigned long rightUntil = 0;
};

// ============================================================
//                      Game Objects + Collision
// ============================================================

class GameObject {
public:
  int16_t x = 0;
  int16_t y = 0;
  int hp = 0;
  
  // Domyślny konstruktor obiektu gry – pozostawia pola z wartościami domyślnymi.
  GameObject() = default; 

  // Wirtualny destruktor, aby poprawnie niszczyć obiekty przez wskaźniki bazowe.
  virtual ~GameObject() = default;

  // Kopiujący konstruktor 
  GameObject(const GameObject&) = default;

  // Kopiujący operator przypisania 
  GameObject& operator=(const GameObject&) = default;

  // Przenoszący konstruktor 
  GameObject(GameObject&&) noexcept = default;

  // Przenoszący operator przypisania 
  GameObject& operator=(GameObject&&) noexcept = default;

  // Wirtualna metoda aktualizująca stan obiektu (np. ruch pocisku, wroga).
  virtual void objectUpdate() = 0;

  // Wirtualna metoda sprawdzająca, czy obiekt zajmuje dany piksel w świecie.
  virtual bool occupiesPixel(int16_t worldX, int16_t worldY) const = 0;

  // Wirtualny getter szerokości obiektu (w pikselach).
  virtual int16_t getWidth()  const = 0;

  // Wirtualny getter wysokości obiektu (w pikselach).
  virtual int16_t getHeight() const = 0;

  // Zmniejsza HP obiektu o wartość damage, ale nie poniżej zera.
  void reduceHp(int damage) {
    hp -= damage;
    if (hp < 0) hp = 0;
  }

  // Zwraca bieżące HP obiektu, sprowadzone do minimum 0.
  int hpAsInt() const { return (hp < 0) ? 0 : hp; }
protected:
  void enforceOnscreen(int16_t sx, int16_t sy, int16_t w, int16_t h) {
    if (sx < 0 || sy < 0 ||
        sx + w > SCREEN_WIDTH ||
        sy + h > SCREEN_HEIGHT) {
      throw OffscreenObjectError{};
    }
  }

};

// Zwraca true, jeśli HP obiektu jest mniejsze lub równe 0 (obiekt martwy).
static inline bool isDead(const GameObject& obj) { return obj.hp <= 0; }

// Sprawdza kolizję piksel-po-pikselu pomiędzy dwoma obiektami (a i b),
// wykorzystując ich bounding box i metodę occupiesPixel(...).
static bool pixelCollision(const GameObject& a, const GameObject& b) {
  int16_t aL = a.x;
  int16_t aR = a.x + a.getWidth()  - 1;
  int16_t aT = a.y;
  int16_t aB = a.y + a.getHeight() - 1;

  int16_t bL = b.x;
  int16_t bR = b.x + b.getWidth()  - 1;
  int16_t bT = b.y;
  int16_t bB = b.y + b.getHeight() - 1;

  if (aR < bL || bR < aL || aB < bT || bB < aT) return false;

  int16_t left   = (aL > bL) ? aL : bL;
  int16_t right  = (aR < bR) ? aR : bR;
  int16_t top    = (aT > bT) ? aT : bT;
  int16_t bottom = (aB < bB) ? aB : bB;

  for (int16_t yy = top; yy <= bottom; yy++) {
    for (int16_t xx = left; xx <= right; xx++) {
      if (a.occupiesPixel(xx, yy) && b.occupiesPixel(xx, yy)) return true;
    }
  }
  return false;
}

// ============================================================
//                      Bullets
// ============================================================

class PlayerBullet : public GameObject {
public:
  static const int MODEL_SIZE = 3;
  static const int SPEED = 5;

  int8_t model[MODEL_SIZE][MODEL_SIZE] = {
    {1,1,1},
    {1,1,1},
    {1,1,1}
  };

  // Konstruktor pocisku gracza: ustawia pozycję startową i HP=1.
  PlayerBullet(int16_t startX, int16_t startY) {
    enforceOnscreen(startX, startY, MODEL_SIZE, MODEL_SIZE);
    x = startX;
    y = startY;
    hp = 1;
  }

  // Aktualizacja stanu pocisku: przemieszcza go w górę ekranu o stałą prędkość SPEED.
  void objectUpdate() override { y -= SPEED; }

  // Rysuje pocisk w buforze board, używając wartości 11 w pikselach modelu.
  void drawBullet(uint8_t board[SCREEN_HEIGHT][SCREEN_WIDTH]) {
    for (int row = 0; row < MODEL_SIZE; row++) {
      for (int col = 0; col < MODEL_SIZE; col++) {
        if (model[row][col] == 0) continue;
        int16_t px = x + col;
        int16_t py = y + row;
        if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) board[py][px] = 11;
      }
    }
  }

  // Zwraca true, jeśli pocisk całkowicie opuścił górną krawędź ekranu.
  bool isOffScreen() const { return (y + MODEL_SIZE <= 0); }

  // Określa, czy pocisk zajmuje dany piksel świata (uwzględnia model 3x3).
  bool occupiesPixel(int16_t worldX, int16_t worldY) const override {
    int16_t lx = worldX - x;
    int16_t ly = worldY - y;
    if (lx < 0 || lx >= MODEL_SIZE) return false;
    if (ly < 0 || ly >= MODEL_SIZE) return false;
    return model[ly][lx] != 0;
  }

  // Zwraca szerokość pocisku (3 piksele).
  int16_t getWidth()  const override { return MODEL_SIZE; }

  // Zwraca wysokość pocisku (3 piksele).
  int16_t getHeight() const override { return MODEL_SIZE; }
};

// Enemy bullet 4x4, w dół
class EnemyBullet : public GameObject {
public:
  static constexpr int MODEL_SIZE = 4;
  static constexpr int ENEMY_BULLET_SPEED_Y = 3;

  // Konstruktor pocisku wroga: ustawia pozycję startową i HP=1.
  EnemyBullet(int16_t startX, int16_t startY) {
    enforceOnscreen(startX, startY, MODEL_SIZE, MODEL_SIZE);
    x = startX;
    y = startY;
    hp = 1;
  }

  // Destruktor informuje na Serialu, że pocisk został usunięty (trafienie lub wyjście poza ekran).
  ~EnemyBullet() override {
    Serial.println("EnemyBullet destructor (usuniety - trafienie lub poza ekranem).");
  }
  
  // Konstruktor kopiujący, operator przypisania, konstruktor przenoszący i operator przenoszący
  EnemyBullet(const EnemyBullet&) = default;
  EnemyBullet& operator=(const EnemyBullet&) = default;
  EnemyBullet(EnemyBullet&&) noexcept = default;
  EnemyBullet& operator=(EnemyBullet&&) noexcept = default;

  // Aktualizuje pozycję pocisku wroga, przesuwając go w dół o ENEMY_BULLET_SPEED_Y.
  void objectUpdate() override { 
    y += ENEMY_BULLET_SPEED_Y; 
  }

  // Rysuje pocisk wroga w buforze board, wypełniając kwadrat 4x4 wartością 12.
  void drawBullet(uint8_t board[SCREEN_HEIGHT][SCREEN_WIDTH]) {
    for (int row = 0; row < MODEL_SIZE; row++) {
      for (int col = 0; col < MODEL_SIZE; col++) {
        int16_t px = x + col;
        int16_t py = y + row;
        if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) board[py][px] = 12;
      }
    }
  }

  // Zwraca true, jeśli pocisk wroga opuścił dolną krawędź ekranu.
  bool isOffScreenDown() const { 
    return y >= SCREEN_HEIGHT; 
  }

  // Określa, czy pocisk wroga zajmuje dany piksel (pełny kwadrat 4x4).
  bool occupiesPixel(int16_t worldX, int16_t worldY) const override {
    int16_t lx = worldX - x;
    int16_t ly = worldY - y;
    if (lx < 0 || lx >= MODEL_SIZE) return false;
    if (ly < 0 || ly >= MODEL_SIZE) return false;
    return true;
  }

  // Zwraca szerokość pocisku wroga (4 piksele).
  int16_t getWidth()  const override { 
    return MODEL_SIZE; 
  }

  // Zwraca wysokość pocisku wroga (4 piksele).
  int16_t getHeight() const override { 
    return MODEL_SIZE; 
  }
};

// ============================================================
//                      Player
// ============================================================

class Player : public GameObject {
public:
  // Konstruktor gracza – inicjuje jego pozycję i HP, wywołując reset().
  Player() { reset(); }

  // Ustawia gracza na dole ekranu, wyśrodkowanego w poziomie,
  // oraz przydziela mu startowe HP.
  void reset() {
    x = (SCREEN_WIDTH  - MODEL_WIDTH) / 2;
    y = (SCREEN_HEIGHT - MODEL_HEIGHT);
    hp = 10;
  }

  void objectUpdate() override {}

  // Rysuje statek gracza w buforze board, według tablicy model 12x19 (wartość 1).
  void drawPlayer(uint8_t board[SCREEN_HEIGHT][SCREEN_WIDTH]) {
    for (int row = 0; row < MODEL_HEIGHT; row++) {
      for (int col = 0; col < MODEL_WIDTH; col++) {
        if (model[row][col] == 0) continue;
        int16_t px = x + col;
        int16_t py = y + row;
        if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) board[py][px] = 1;
      }
    }
  }

  // Przesuwa gracza o MOVE_STEP w lewo, nie wychodząc poza ekran.
  void TurnLeft()  { 
    x = (x >= MOVE_STEP) ? (x - MOVE_STEP) : 0; 
  }

  // Przesuwa gracza o MOVE_STEP w prawo, nie pozwalając wyjść poza ekran.
  void TurnRight() { 
    x = (x + MODEL_WIDTH + MOVE_STEP <= SCREEN_WIDTH) ? (x + MOVE_STEP) : (SCREEN_WIDTH - MODEL_WIDTH); 
  }

  // Tworzy pocisk gracza przy lewym skraju statku, nad jego górną krawędzią.
  std::unique_ptr<PlayerBullet> shootLeftEdge() {
    const int16_t bulletX = x;
    const int16_t bulletY = y - PlayerBullet::MODEL_SIZE;
    return std::make_unique<PlayerBullet>(bulletX, bulletY);
  }

  // Tworzy pocisk gracza przy prawym skraju statku, nad jego górną krawędzią.
  std::unique_ptr<PlayerBullet> shootRightEdge() {
    const int16_t bulletX = x + (MODEL_WIDTH - PlayerBullet::MODEL_SIZE);
    const int16_t bulletY = y - PlayerBullet::MODEL_SIZE;
    return std::make_unique<PlayerBullet>(bulletX, bulletY);
  }

  // Zwraca współrzędną Y górnej krawędzi modelu gracza.
  int16_t getTopY() const { return y; }

  // Zwraca współrzędną X środka modelu gracza.
  int16_t centerX() const { return x + MODEL_WIDTH / 2; }

  // Określa, czy gracz zajmuje dany piksel świata (sprawdza maskę modelu 12x19).
  bool occupiesPixel(int16_t worldX, int16_t worldY) const override {
    int16_t lx = worldX - x;
    int16_t ly = worldY - y;
    if (lx < 0 || lx >= MODEL_WIDTH)  return false;
    if (ly < 0 || ly >= MODEL_HEIGHT) return false;
    return model[ly][lx] != 0;
  }

  // Zwraca szerokość modelu gracza (19 pikseli).
  int16_t getWidth()  const override { return MODEL_WIDTH;  }

  // Zwraca wysokość modelu gracza (12 pikseli).
  int16_t getHeight() const override { return MODEL_HEIGHT; }

private:
  static constexpr int MOVE_STEP = 4;
  static constexpr int MODEL_HEIGHT = 12;
  static constexpr int MODEL_WIDTH  = 19;

  int8_t model[MODEL_HEIGHT][MODEL_WIDTH] = {
    {0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0},
    {0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0},
    {0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
    {0,0,0,0,0,1,0,1,0,1,0,1,0,1,0,0,0,0,0},
    {0,0,0,0,1,0,0,1,0,0,1,0,0,1,0,0,0,0,0},
    {0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0},
    {0,0,0,0,1,1,0,1,1,0,1,1,0,0,0,0,0,0,0}
  };
};

// ============================================================
//                      Enemies
// ============================================================

class Enemy : public GameObject {
public:
  static constexpr int MODEL_HEIGHT = 16;
  static constexpr int MODEL_WIDTH  = 16;

  // Domyślny konstruktor – ustawia wroga w (0,0) z HP=1.
  Enemy() { 
    x = 0; 
    y = 0; 
    hp = 1; }

  // Konstruktor z pozycją startową – ustawia wroga w (startX,startY) z HP=1.
  Enemy(int16_t startX, int16_t startY) { 
    enforceOnscreen(startX, startY, MODEL_WIDTH, MODEL_HEIGHT);
    x = startX; 
    y = startY; 
    hp = 1; }

  // Destruktor wypisuje na Serialu informację o usunięciu wroga.
  ~Enemy() override { Serial.println("Enemy destructor (usuniety albo zszedl z ekranu)."); }

  //operacje kopiowania/przenoszenia.
  Enemy(const Enemy&) = default;
  Enemy& operator=(const Enemy&) = default;
  Enemy(Enemy&&) noexcept = default;
  Enemy& operator=(Enemy&&) noexcept = default;

  // Aktualizacja stanu wroga – tutaj nic nie robi, ruch obsługuje osobna metoda.
  void objectUpdate() override {}

  // Przesuwa wroga w dół ekranu o 2 piksele.
  void moveDownEnemy() { 
    y += 2; 
  }

  // Zwraca true, jeśli wróg opuścił dolną krawędź ekranu.
  bool isOffScreenDown() const { return y >= SCREEN_HEIGHT; }

  // Zwraca true, jeśli wróg już zadał obrażenia graczowi przy kontakcie.
  bool alreadyDamagedPlayer() const { return hasDamagedPlayer; }

  // Oznacza, że wróg zadał już obrażenia graczowi (aby nie liczyć ponownie).
  void markDamagedPlayer() { hasDamagedPlayer = true; }

  // Rysuje zwykłego wroga na planszy, używając modelu 16x16 i wartości 2.
  void drawEnemy(uint8_t board[SCREEN_HEIGHT][SCREEN_WIDTH]) {
    for (int row = 0; row < MODEL_HEIGHT; row++) {
      for (int col = 0; col < MODEL_WIDTH; col++) {
        if (model[row][col] == 0) continue;
        int16_t px = x + col;
        int16_t py = y + row;
        if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) board[py][px] = 2;
      }
    }
  }

  // Określa, czy wróg zajmuje dany piksel (na podstawie maski 16x16).
  bool occupiesPixel(int16_t worldX, int16_t worldY) const override {
    int16_t lx = worldX - x;
    int16_t ly = worldY - y;
    if (lx < 0 || lx >= MODEL_WIDTH)  return false;
    if (ly < 0 || ly >= MODEL_HEIGHT) return false;
    return model[ly][lx] != 0;
  }

  // Zwraca szerokość modelu  wroga (16 pikseli).
  int16_t getWidth()  const override { return MODEL_WIDTH;  }

  // Zwraca wysokość modelu wroga (16 pikseli).
  int16_t getHeight() const override { return MODEL_HEIGHT; }

private:
  bool hasDamagedPlayer = false;

  int8_t model[MODEL_HEIGHT][MODEL_WIDTH] = {
    {0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0},
    {0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0},
    {1,1,0,0,0,1,1,1,1,1,0,0,0,1,1,0},
    {1,1,0,0,0,1,1,1,1,1,0,0,0,1,1,0},
    {1,1,0,0,0,1,1,1,1,1,0,0,0,1,1,0},
    {1,1,0,0,0,1,1,1,1,1,0,0,0,1,1,0},
    {0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
    {0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
    {0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0},
    {0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0}
  };
};

class EnemyShooter : public GameObject {
public:
  static constexpr int MODEL_HEIGHT = 16;
  static constexpr int MODEL_WIDTH  = 16;
  static constexpr unsigned long ENEMY_SHOOTER_COOLDOWN_MS = 1000;

  // Domyślny konstruktor – pozycja (0,0), HP=1.
  EnemyShooter() { 
    x = 0; 
    y = 0; 
    hp = 1; }

  // Konstruktor z pozycją startową – ustawia shooter w danym X,Y.
  EnemyShooter(int16_t startX, int16_t startY) { 
    enforceOnscreen(startX, startY, MODEL_WIDTH, MODEL_HEIGHT);
    x = startX; 
    y = startY; 
    hp = 1; }

  // Destruktor wypisuje informację o usunięciu shootera.
  ~EnemyShooter() override { Serial.println("EnemyShooter destructor (usuniety albo zszedl z ekranu)."); }

  // operacje kopiowania/przenoszenia
  EnemyShooter(const EnemyShooter&) = default;
  EnemyShooter& operator=(const EnemyShooter&) = default;
  EnemyShooter(EnemyShooter&&) noexcept = default;
  EnemyShooter& operator=(EnemyShooter&&) noexcept = default;

  void objectUpdate() override {}

  // Przesuwa shootera w dół o 1 piksel.
  void moveDownEnemy() { y += 1; }

  // Zwraca true, jeśli shooter wyszedł poza dolną krawędź ekranu.
  bool isOffScreenDown() const { return y >= SCREEN_HEIGHT; }

  // Sprawdza, czy shooter już zadał obrażenia graczowi.
  bool alreadyDamagedPlayer() const { return hasDamagedPlayer; }

  // Oznacza, że shooter zadał już obrażenia graczowi (brak ponownych trafień przy kolizji).
  void markDamagedPlayer() { hasDamagedPlayer = true; }

  // Rysuje shootera w buforze board, używając wartości 3 i maski 16x16.
  void drawEnemy(uint8_t board[SCREEN_HEIGHT][SCREEN_WIDTH]) {
    for (int row = 0; row < MODEL_HEIGHT; row++) {
      for (int col = 0; col < MODEL_WIDTH; col++) {
        if (model[row][col] == 0) continue;
        int16_t px = x + col;
        int16_t py = y + row;
        if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) board[py][px] = 3;
      }
    }
  }

  // Próbuje oddać strzał, jeśli minął cooldown:
  // tworzy nowy EnemyBullet na środku shootera i dopisuje go do outBullets.
  void tryShoot(unsigned long now, std::vector<std::unique_ptr<EnemyBullet>>& outBullets) {
    if (now - lastShotMs < ENEMY_SHOOTER_COOLDOWN_MS) return;
    lastShotMs = now;

    int16_t spawnX = x + (MODEL_WIDTH  - EnemyBullet::MODEL_SIZE) / 2;
    int16_t spawnY = y + MODEL_HEIGHT;
    outBullets.push_back(std::make_unique<EnemyBullet>(spawnX, spawnY));
  }

  // Określa, czy shooter zajmuje dany piksel (na podstawie maski 16x16).
  bool occupiesPixel(int16_t worldX, int16_t worldY) const override {
    int16_t lx = worldX - x;
    int16_t ly = worldY - y;
    if (lx < 0 || lx >= MODEL_WIDTH)  return false;
    if (ly < 0 || ly >= MODEL_HEIGHT) return false;
    return model[ly][lx] != 0;
  }

  // Zwraca szerokość modelu shootera (16 pikseli).
  int16_t getWidth()  const override { return MODEL_WIDTH;  }

  // Zwraca wysokość modelu shootera (16 pikseli).
  int16_t getHeight() const override { return MODEL_HEIGHT; }

private:
  bool hasDamagedPlayer = false;
  unsigned long lastShotMs = 0;

  int8_t model[MODEL_HEIGHT][MODEL_WIDTH] = {
    {0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0},
    {0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0},
    {0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
    {0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0},
    {0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0},
    {0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
    {0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0},
    {0,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
  };
};

class EnemyTank : public GameObject {
public:
  static constexpr int MODEL_HEIGHT = 16;
  static constexpr int MODEL_WIDTH  = 16;

  // Domyślny konstruktor – tank w (0,0) z HP=3.
  EnemyTank() { 
    x = 0; 
    y = 0; 
    hp = 3; }

  // Konstruktor z pozycją startową – tank w (startX,startY) z HP=3.
  EnemyTank(int16_t startX, int16_t startY) { 
    enforceOnscreen(startX, startY, MODEL_WIDTH, MODEL_HEIGHT);
    x = startX; 
    y = startY; 
    hp = 3; }

  // Destruktor wypisuje na Serialu informację o usunięciu czołgu.
  ~EnemyTank() override { Serial.println("EnemyTank destructor (usuniety albo zszedl z ekranu)."); }

  //operacje kopiowania/przenoszenia.
  EnemyTank(const EnemyTank&) = default;
  EnemyTank& operator=(const EnemyTank&) = default;
  EnemyTank(EnemyTank&&) noexcept = default;
  EnemyTank& operator=(EnemyTank&&) noexcept = default;

  // Aktualizacja stanu czołgu – nic nie robi, ruch jest w moveDownEnemy().
  void objectUpdate() override {}

  // Przesuwa czołg o 1 piksel w dół ekranu.
  void moveDownEnemy() { y += 1; }

  // Sprawdza, czy czołg wyszedł poza dolną krawędź ekranu.
  bool isOffScreenDown() const { return y >= SCREEN_HEIGHT; }

  // Sprawdza, czy czołg już zadał obrażenia graczowi.
  bool alreadyDamagedPlayer() const { return hasDamagedPlayer; }

  // Oznacza, że czołg zadał już obrażenia graczowi.
  void markDamagedPlayer() { hasDamagedPlayer = true; }

  // Rysuje czołg w buforze board, wypełniając jego maskę 16x16 wartością 4.
  void drawEnemy(uint8_t board[SCREEN_HEIGHT][SCREEN_WIDTH]) {
    for (int row = 0; row < MODEL_HEIGHT; row++) {
      for (int col = 0; col < MODEL_WIDTH; col++) {
        int16_t px = x + col;
        int16_t py = y + row;
        if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) board[py][px] = 4;
      }
    }
  }

  // Określa, czy czołg zajmuje dany piksel (cały prostokąt 16x16).
  bool occupiesPixel(int16_t worldX, int16_t worldY) const override {
    const int16_t lx = worldX - x;
    const int16_t ly = worldY - y;
    if (lx < 0 || lx >= MODEL_WIDTH)  return false;
    if (ly < 0 || ly >= MODEL_HEIGHT) return false;
    return true;
  }

  // Zwraca szerokość czołgu (16 pikseli).
  int16_t getWidth()  const override { return MODEL_WIDTH;  }

  // Zwraca wysokość czołgu (16 pikseli).
  int16_t getHeight() const override { return MODEL_HEIGHT; }

private:
  bool hasDamagedPlayer = false;
};

// ============================================================
//                      GameEngine
// ============================================================

class GameEngine {
public:
  // Konstruktor silnika gry:
  // - tworzy obiekty pomocnicze (screen, pins, player),
  // - rezerwuje pamięć w wektorach pocisków/wrogów,
  // - inicjalizuje tablicę blokad linii spawnu.
  GameEngine(Adafruit_SSD1306& disp)
  : screen(disp), pins(36, 34, 33, 27), player() {
    bullets.reserve(12);
    enemyBullets.reserve(24);
    enemies.reserve(MAX_ENEMIES);
    specialLaneLocked.fill(false);
  }

  // Inicjalizuje piny sterujące przyciskami i system HP/buzzera.
  // Nazwa zmieniona z begin() na initializeEngine().
  void initializeEngine() {
    pins.configureInputs();
    hpAndShoots.initializeOutputs();
  }

  // Główna, uproszczona pętla gry:
  // - resetuje stan gry,
  // - w pętli wywołuje pomocnicze funkcje:
  //   * przygotowanie ramki (timery buzzerów + czyszczenie bufora),
  //   * ruch gracza,
  //   * strzały gracza,
  //   * spawn wrogów,
  //   * aktualizacja wrogów,
  //   * aktualizacja pocisków wroga,
  //   * aktualizacja pocisków gracza i kolizji z wrogami,
  //   * rysowanie ramki,
  // - kończy grę po śmierci gracza i wyświetla ekran wyniku.
  void gameLoop() {
    startNewGame();

    unsigned long lastShot       = 0;
    unsigned long lastEnemySpawn = millis();
    unsigned long lastEnemyMove  = millis();

    bool playerDied = false;

    while (game_status) {
      const unsigned long now = millis();

      prepareFrame();
      handlePlayerMovement();
      handlePlayerShooting(now, lastShot);
      spawnEnemiesIfNeeded(now, lastEnemySpawn);
      updateEnemiesIfNeeded(now, lastEnemyMove);
      updateEnemyBullets();
      updatePlayerBulletsAndEnemies();
      drawFrame();

      if (isDead(player)) {
        playerDied = true;
        game_status = false;
      }
    }

    if (playerDied) {
      hpAndShoots.stopAll();
      hpAndShoots.setHpLeds(0);
      showScoreScreenForMs(8000);
    }
  }

private:
  static constexpr size_t MAX_ENEMIES = 12;
  static constexpr unsigned long ENEMY_SPAWN_INTERVAL_MS = 2000;
  static constexpr unsigned long SHOT_COOLDOWN_MS        = 1000;
  static constexpr unsigned long ENEMY_MOVE_INTERVAL_MS  = 200;

  static constexpr int TANK_SPAWN_CHANCE_PERCENT    = 20;
  static constexpr int SHOOTER_SPAWN_CHANCE_PERCENT = 30;

  static constexpr std::array<int16_t, 8> SPAWN_X = { 0, 16, 32, 48, 64, 80, 96, 112 };

  using EnemyVariant = std::variant<Enemy, EnemyShooter, EnemyTank>;
  using EnemyHandle  = std::unique_ptr<EnemyVariant>;

  std::array<bool, SPAWN_X.size()> specialLaneLocked{};

  // Przygotowuje jedną klatkę do rysowania:
  // - aktualizuje buzzery,
  // - czyści board.
  void prepareFrame() {
    hpAndShoots.update();
    screen.clearBoard(board);
  }

  // Odczytuje przyciski ruchu i przesuwa gracza w lewo/prawo.
  void handlePlayerMovement() {
    if (pins.left())  player.TurnLeft();
    if (pins.right()) player.TurnRight();
  }

  // Obsługuje strzały gracza:
  // - pilnuje cooldownu SHOT_COOLDOWN_MS,
  // - w razie strzału tworzy pociski z lewej/prawej krawędzi statku,
  // - uruchamia odpowiednie dźwięki w HpAndShoots.
  void handlePlayerShooting(unsigned long now, unsigned long& lastShot) {
    if (now - lastShot < SHOT_COOLDOWN_MS) return;

    const bool leftShot  = pins.shootLeft();
    const bool rightShot = pins.shootRight();
    if (!leftShot && !rightShot) return;

    if (leftShot)  {
      bullets.push_back(player.shootLeftEdge());
      hpAndShoots.playShootLeft();
    }
    if (rightShot) {
      bullets.push_back(player.shootRightEdge());
      hpAndShoots.playShootRight();
    }

    lastShot = now;
  }

  // Próbuje zespawnować wrogów (z obsługą EnemyOverflowError),
  // jeśli minął odpowiedni interwał czasu.
  void spawnEnemiesIfNeeded(unsigned long now, unsigned long& lastEnemySpawn) {
    if (now - lastEnemySpawn < ENEMY_SPAWN_INTERVAL_MS) return;

    try {
      trySpawnEnemy(now, lastEnemySpawn);
    } catch (const EnemyOverflowError& e) {
      Serial.println(e.what());
      // Ignorujemy – gra działa, ale nie spawnujemy nowych.
    }
  }

  // Aktualizuje ruch wrogów co ENEMY_MOVE_INTERVAL_MS:
  // - przesuwa ich w dół,
  // - sprawdza kolizję kontaktową z graczem,
  // - obsługuje strzały EnemyShooterów,
  // - usuwa tych, którzy zeszli poza ekran (i zabiera HP graczowi).
  void updateEnemiesIfNeeded(unsigned long now, unsigned long& lastEnemyMove) {
    if (now - lastEnemyMove < ENEMY_MOVE_INTERVAL_MS) return;

    lastEnemyMove = now;
    moveEnemiesTowardPlayerAndHandleDamage(now);
    removeEnemiesThatLeftScreen();
  }

  // Aktualizuje wszystkie pociski wroga:
  // - przesuwa je,
  // - usuwa te, które wyszły poza ekran,
  // - wykrywa kolizję z graczem (zabiera HP, usuwa pocisk),
  // - rysuje pozostałe pociski.
  void updateEnemyBullets() {
    for (auto it = enemyBullets.begin(); it != enemyBullets.end(); ) {
      (*it)->objectUpdate();

      if ((*it)->isOffScreenDown()) {
        it = enemyBullets.erase(it);
        continue;
      }

      if (pixelCollision(**it, player)) {
        player.reduceHp(1);
        hpAndShoots.setHpLeds(player.hpAsInt());
        it = enemyBullets.erase(it);
        continue;
      }

      (*it)->drawBullet(board);
      ++it;
    }
  }

  // Aktualizuje pociski gracza oraz ich kolizje z wrogami:
  // - przesuwa pociski,
  // - usuwa te, które wyszły poza ekran,
  // - sprawdza kolizję z każdym wrogiem,
  // - przy trafieniu zadaje obrażenia, usuwa pocisk,
  // - jeśli wróg ginie:
  //   * odblokowuje linię jeśli wróg był specjalny,
  //   * zwiększa wynik z zabezpieczeniem na overflow,
  //   * usuwa wroga.
  void updatePlayerBulletsAndEnemies() {
    for (auto bit = bullets.begin(); bit != bullets.end(); ) {
      (*bit)->objectUpdate();

      if ((*bit)->isOffScreen()) {
        bit = bullets.erase(bit);
        continue;
      }

      bool bulletRemoved = false;

      for (auto eit = enemies.begin(); eit != enemies.end(); ) {
        bool collided = std::visit([&](auto& enemyObj) {
          return pixelCollision(**bit, enemyObj);
        }, **eit);

        if (!collided) {
          ++eit;
          continue;
        }

        // Pocisk trafił – usuwamy pocisk i zadajemy obrażenia wrogowi.
        bit = bullets.erase(bit);
        bulletRemoved = true;

        std::visit([&](auto& enemyObj) { enemyObj.reduceHp(1); }, **eit);

        bool dead = std::visit([&](auto& enemyObj) { return isDead(enemyObj); }, **eit);
        if (dead) {
          unlockLaneIfSpecial(**eit);

          // zwiększanie wyniku z zabezpieczeniem na overflow
          try {
            if (score >= std::numeric_limits<unsigned long long>::max()) {
              throw ScoreOverflowError{};
            }
            ++score;
          } catch (const ScoreOverflowError& e) {
            Serial.println(e.what());
            // nie zwiększamy już bardziej wyniku
          }

          eit = enemies.erase(eit);
        } else {
          ++eit;
        }

        // Ponieważ pocisk już został usunięty po pierwszym trafieniu, przerywamy wewnętrzną pętlę.
        break;
      }

      if (!bulletRemoved) {
        (*bit)->drawBullet(board);
        ++bit;
      }
    }
  }

  // Rysuje pełną scenę:
  // - rysuje gracza,
  // - rysuje wszystkich wrogów,
  // - renderuje bufor na ekran z obrotem 180 stopni.
  void drawFrame() {
    player.drawPlayer(board);
    for (auto& ePtr : enemies) {
      std::visit([&](auto& e) { e.drawEnemy(board); }, *ePtr);
    }

    screen.render(board, true);
  }

  // Resetuje całą grę:
  // - zeruje wynik, ustawia status gry,
  // - czyści wszystkie kontenery z pociskami i wrogami,
  // - odblokowuje specjalne linie,
  // - resetuje gracza,
  // - aktualizuje diody HP i czyści/odświeża ekran.
  void startNewGame() {
    score = 0;
    game_status = true;
    bullets.clear();
    enemyBullets.clear();
    enemies.clear();
    specialLaneLocked.fill(false);
    player.reset();

    hpAndShoots.setHpLeds(player.hpAsInt());
    screen.clearBoard(board);
    screen.render(board, true);
  }

  // Sprawdza, czy dwa prostokąty (ax,ay,aw,ah) i (bx,by,bw,bh) nachodzą się na siebie.
  static bool rectsOverlap(int16_t ax, int16_t ay, int16_t aw, int16_t ah,
                           int16_t bx, int16_t by, int16_t bw, int16_t bh) {
    return (ax < bx + bw) && (ax + aw > bx) && (ay < by + bh) && (ay + ah > by);
  }

  static constexpr int16_t ENEMY_W = 16;
  static constexpr int16_t ENEMY_H = 16;

  // Sprawdza, czy można zespawnować wroga w danej linii:
  // - linia nie jest zablokowana jako specjalna,
  // - w danym miejscu nie koliduje z istniejącymi wrogami.
  bool canSpawnAt(int laneIdx, int16_t sx, int16_t sy) const {
    if (laneIdx >= 0 && laneIdx < (int)specialLaneLocked.size() && specialLaneLocked[laneIdx]) {
      return false;
    }

    return !std::any_of(enemies.begin(), enemies.end(),
      [&](const EnemyHandle& ePtr) {
        return std::visit([&](const auto& e) {
          return rectsOverlap(sx, sy, ENEMY_W, ENEMY_H, e.x, e.y, e.getWidth(), e.getHeight());
        }, *ePtr);
      }
    );
  }

  // Na podstawie losowej liczby r0_99 wybiera typ wroga:
  // 2 – czołg (EnemyTank), 1 – shooter (EnemyShooter), 0 – zwykły (Enemy).
  int pickEnemyTypeRoll(int r0_99) const {
    if (r0_99 < TANK_SPAWN_CHANCE_PERCENT) return 2; // Tank
    if (r0_99 < TANK_SPAWN_CHANCE_PERCENT + SHOOTER_SPAWN_CHANCE_PERCENT) return 1; // Shooter
    return 0; // Normal
  }

  // Dla specjalnych typów wroga (shooter/tank) blokuje linię, w której się pojawili.
  void lockLaneIfSpecial(int type, int laneIdx) {
    if (laneIdx < 0 || laneIdx >= (int)specialLaneLocked.size()) return;
    if (type == 1 || type == 2) specialLaneLocked[laneIdx] = true;
  }

  // Wylicza indeks linii (0–7) na podstawie współrzędnej X (co 16 pikseli).
  static int laneFromX(int16_t x) {
    int lane = x / 16;
    if (lane < 0) lane = 0;
    if (lane > 7) lane = 7;
    return lane;
  }

  // Odblokowuje linię, jeśli dany wariant wroga był typem specjalnym (shooter/tank).
  void unlockLaneIfSpecial(const EnemyVariant& v) {
    if (std::holds_alternative<Enemy>(v)) return;
    const int lane = laneFromX(std::visit([](const auto& e){ return e.x; }, v));
    if (lane >= 0 && lane < (int)specialLaneLocked.size()) specialLaneLocked[lane] = false;
  }

  // Próbuje zespawnować nowego wroga:
  // - pilnuje interwału czasowego spawnu,
  // - jeśli enemies.size() >= MAX_ENEMIES rzuca EnemyOverflowError,
  // - losuje kolejność linii spawnu,
  // - dla pierwszej dostępnej linii tworzy odpowiedni typ wroga (zwykły, shooter, tank),
  //   ewentualnie blokuje linię jako specjalną i aktualizuje czas ostatniego spawnu.
  void trySpawnEnemy(unsigned long now, unsigned long& lastSpawn) {
    if (now - lastSpawn < ENEMY_SPAWN_INTERVAL_MS) return;

    if (enemies.size() >= MAX_ENEMIES) {
      throw EnemyOverflowError{};
    }

    constexpr int16_t spawnY = 0;

    std::array<uint8_t, SPAWN_X.size()> order{};
    std::iota(order.begin(), order.end(), (uint8_t)0);
    for (int i = (int)order.size() - 1; i > 0; --i) {
      int j = random(0, i + 1);
      std::swap(order[i], order[j]);
    }

    for (uint8_t idx : order) {
      const int laneIdx = (int)idx;
      const int16_t spawnX = SPAWN_X[idx];

      if (!canSpawnAt(laneIdx, spawnX, spawnY)) continue;

      const int r = random(0, 100);
      const int type = pickEnemyTypeRoll(r);

      if (type == 2) {
        enemies.push_back(std::make_unique<EnemyVariant>(EnemyTank{spawnX, spawnY}));
        lockLaneIfSpecial(type, laneIdx);
      } else if (type == 1) {
        enemies.push_back(std::make_unique<EnemyVariant>(EnemyShooter{spawnX, spawnY}));
        lockLaneIfSpecial(type, laneIdx);
      } else {
        enemies.push_back(std::make_unique<EnemyVariant>(Enemy{spawnX, spawnY}));
      }

      lastSpawn = now;
      return;

    }
  }

  // Natychmiast zabija gracza:
  // - pobiera aktualne HP,
  // - jeśli >0, odejmuje całe HP (reduceHp(hpNow)),
  // - gasi wszystkie diody HP.
  void killPlayerNow() {
    const int hpNow = player.hpAsInt();
    if (hpNow > 0) {
      player.reduceHp(hpNow);     // kontakt = zabiera całe HP
      hpAndShoots.setHpLeds(0);
    }
  }

  // Przesuwa wszystkich wrogów w stronę gracza, obsługuje kolizję kontaktową z graczem
  // (natychmiastowa śmierć przy zderzeniu), oraz strzały EnemyShooterów.
  void moveEnemiesTowardPlayerAndHandleDamage(unsigned long now) {
    auto killIfCollide = [&](auto& e) {
      if (!e.alreadyDamagedPlayer() && pixelCollision(player, e)) {
        killPlayerNow();
        e.markDamagedPlayer();
      }
    };

    for (auto& ePtr : enemies) {
      // przed ruchem – sprawdzamy kolizję z graczem
      std::visit(killIfCollide, *ePtr);

      // ruch (w dół) – zależny od typu wroga
      std::visit([&](auto& e) { e.moveDownEnemy(); }, *ePtr);

      // po ruchu – ponowna kontrola kolizji
      std::visit(killIfCollide, *ePtr);

      // strzały shooterów
      if (auto* shooter = std::get_if<EnemyShooter>(&(*ePtr))) {
        shooter->tryShoot(now, enemyBullets);
      }
    }
  }

  // Usuwa z listy wszystkich wrogów, którzy opuścili dolną krawędź ekranu:
  // - odblokowuje linię jeśli wróg był specjalny,
  // - jeśli gracz jeszcze żyje, odejmuje mu 1 HP i aktualizuje diody.
  void removeEnemiesThatLeftScreen() {
    // TERAZ: offscreen enemy => -1 HP (dla wszystkich typów)
    for (auto it = enemies.begin(); it != enemies.end(); ) {
      bool off = std::visit([&](auto& e) { return e.isOffScreenDown(); }, **it);
      if (!off) { ++it; continue; }

      // odblokuj lane jeśli special
      unlockLaneIfSpecial(**it);

      // -1 HP tylko jeśli gracz jeszcze żyje
      if (player.hpAsInt() > 0) {
        player.reduceHp(1);
        hpAndShoots.setHpLeds(player.hpAsInt());
      }

      it = enemies.erase(it);

      // jeśli po -1HP gracz umarł, to i tak pętla główna zaraz to wykryje
    }
  }

  // Wyświetla ekran wyniku (napis SCORE:xxx) przez durationMs milisekund:
  // w pętli aktualizuje buzzery, czyści planszę, rysuje tekst i renderuje go.
  void showScoreScreenForMs(unsigned long durationMs) {
    char msg[64];
    std::snprintf(msg, sizeof(msg), "SCORE:%llu", (unsigned long long)score);

    const unsigned long endAt = millis() + durationMs;

    while (millis() < endAt) {
      hpAndShoots.update();
      screen.clearBoard(board);
      screen.drawText3x5Centered(board, msg, 1);
      screen.render(board, true);
      delay(30);
    }
  }

private:
  unsigned long long score = 0;
  bool game_status = false;

  uint8_t board[SCREEN_HEIGHT][SCREEN_WIDTH]{};

  Screen screen;
  Pins pins;
  Player player;
  HpAndShoots hpAndShoots;

  std::vector<std::unique_ptr<PlayerBullet>> bullets;
  std::vector<std::unique_ptr<EnemyBullet>> enemyBullets;
  std::vector<EnemyHandle> enemies;
};

// ============================================================
//                      Arduino setup/loop
// ============================================================

GameEngine engine(display);


// do wykrywania urzadzen i2c
bool i2cDevicePresent(uint8_t address) {
    Wire.beginTransmission(address);
    return (Wire.endTransmission() == 0); // 0 = OK (ACK)
}
// Funkcja setup:
// - uruchamia port szeregowy,
// - inicjalizuje generator liczb losowych,
// - próbuje zainicjalizować wyświetlacz (w razie błędu rzuca DisplayInitError i zatrzymuje program),
// - czyści i odświeża wyświetlacz,
// - inicjalizuje elementy silnika gry (piny, HP/buzzer).
void setup() {
  Serial.begin(115200);
  Wire.begin();

  try {
    if (!i2cDevicePresent(0x3C)) {   
      throw DisplayInitError{};
    }

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
      throw DisplayInitError{};
    }
  }
  catch (const DisplayInitError& e) {
    Serial.println(e.what());
    Serial.println("Halting system due to missing display.");
    for(;;); // zatrzymanie programu
  }

  try {
    PlayerBullet test(-10, -10);   // <- specjalnie poza ekranem
  }
  catch (const OffscreenObjectError& e) {
    Serial.println("OK: wyjątek działa.");
    Serial.println(e.what());
  }
  display.clearDisplay();
  display.display();
  engine.initializeEngine();
}

// Funkcja loop:
// - uruchamia gameLoop() silnika gry,
// - po zakończeniu gry odczekuje 200 ms.
void loop() {
  engine.gameLoop();
  delay(200);
}
