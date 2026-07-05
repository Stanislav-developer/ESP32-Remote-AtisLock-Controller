/* Remote-Door firmware V1.0
Repository: https://github.com/Stanislav-developer/ESP32-Remote-AtisLock-Controller
Author: Stanislav Turii (GitHub: https://github.com/Stanislav-developer || Youtube: https://www.youtube.com/@TehnoMaisterna)
Date: 2026.05.23

НАЛАШТУВАННЯ ДЛЯ ЗАЛИВКИ ПРОШИВКИ (ESP32-C3):
У Tools:
1. Board: "ESP32C3 Dev Module"
2. USB CDC On Boot: "Enabled" (Обов'язково для Serial Monitor)
3. Решта налаштувань: за замовчуванням

ЯК УВІЙТИ В РЕЖИМ ПРОШИВКИ:
1. Під'єднайте ESP32 до комп'ютера.
2. Затисніть кнопку BOOT.
3. Утримуючи BOOT, натисніть кнопку RESET (1 сек).
4. Відпустіть RESET, а потім відпустіть BOOT.
*/

// Підключення вбудованих бібліотек ядра ESP32
#include <ETH.h> // Для роботи з Ethernet залізом на рівні ядра
#include <SPI.h> // Для зв'язку з W5500 по SPI шині
#include <WiFi.h> // Потрібна для підняття власної точки доступу (AP)
#include <WiFiClientSecure.h> // Нативний захищений SSL клієнт
#include <WebServer.h> // Для роботи веб-сервера конфігурації
#include <DNSServer.h> // Для перенаправлення трафіку (Captive Portal)
#include <Preferences.h> // Для роботи з енергонезалежною пам'яттю
#include <UniversalTelegramBot.h> // Бібліотека Telegram API

// Конфігурація апаратних пінів (ESP32-C3 Super Mini)
#define RELAY_PIN 20     // Керування силовим реле замка (Активний низький)
#define LED_BTN_PIN 21   // Транзистор підсвітки кнопки (Активний високий)
#define BUTTON_PIN 2     // Домофонна кнопка виходу на стіні (INPUT_PULLUP)
#define LED_SYS_PIN 3    // Системний світлодіод індикації статусу (ШІМ сумісний)

// Апаратні піни SPI шини та скидання для Ethernet контролера W5500
#define W5500_SCK 4
#define W5500_MISO 5
#define W5500_MOSI 6
#define W5500_CS 7
#define W5500_RST 1

// Дані конфігурації системи (заповнюються через Веб-Інтерфейс або Preferences)
String apPassword = "";  // Пароль для точки доступу та скидання налаштувань
String botToken = "";    // API Токен Telegram бота
String chatId = "";      // Чат ID адміна (власника системи)
String groupId = "";     // Чат ID робочої групи або каналу

// Об'єкти системних класів
Preferences preferences; // Об'єкт для збереження конфігурації в NVS флеш
WiFiClientSecure client; // Безпечний клієнт із підтримкою апаратного mbedtls
UniversalTelegramBot bot(botToken, client); // Об'єкт обробника Telegram API
WebServer server(80);     // Веб-сервер на стандартному 80 порту
DNSServer dnsServer;     // DNS сервер для реалізації Captive Portal

// --- ГЛОБАЛЬНІ ЗМІННІ СТАНУ ---
bool isAccessAllowed = false; // Дозвіл на відкриття дверей з фізичної кнопки
int openCount = 0;            // Загальна кількість відкривань замка за весь час

// Змінні для асинхронного керування системним світлодіодом (без затримок delay)
unsigned long lastSysLedBlink = 0; // Таймер останнього перемикання стану LED
bool sysLedState = false;          // Поточний стан світлодіода (вкл/викл)
int sysLedInterval = 300; // Інтервал блимання: 300мс - пошук мережі, 1500мс - помилка, 0 - норма

// Змінні захисного таймауту кнопки та реле (блокування від заспамлювання)
bool isButtonCooldown = false;     // Прапорець активного режиму блокування
unsigned long cooldownStartTime = 0; // Час початку таймауту безпеки
unsigned long lastBtnLedToggle = 0; // Таймер миготіння підсвітки кнопки
bool btnLedState = false;          // Поточний стан підсвітки кнопки виходу
const unsigned long COOLDOWN_DURATION = 5000; // Тривалість блокування (5 секунд)

// Змінні авторизації для процедури очищення пам'яті через чат
bool waitingForClearDataPass = false; // Очікування введення пароля в чат
String clearDataRequesterId = "";     // ID користувача, який затребував очищення

// Глобальний прапор успішного підключення Ethernet на рівні lwIP стеку
bool eth_connected = false;

// HTML код розмітки сторінки конфігурації Веб-Інтерфейсу
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
  <title>Remote-Door Setup</title>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; background-color: #0b132b; color: #ffffff; text-align: center; margin: 0; }
    .header { background-color: #1c2541; padding: 20px; border-bottom: 3px solid #3a506b; }
    .container { padding: 20px; max-width: 400px; margin: 0 auto; background-color: #1c2541; border-radius: 8px; margin-top: 20px; }
    input { width: 100%; padding: 12px; margin: 8px 0; box-sizing: border-box; background-color: #0b132b; color: #fff; border: 2px solid #3a506b; border-radius: 4px; }
    input[type=submit] { background-color: #3a506b; color: white; border: none; cursor: pointer; font-weight: bold; font-size: 16px; margin-top: 10px; }
    input[type=submit]:hover { background-color: #5bc0be; color: #0b132b; }
    .footer { margin-top: 30px; font-size: 12px; color: #aaa; border-top: 1px solid #3a506b; padding-top: 15px; }
    a { color: #5bc0be; text-decoration: none; display: block; margin-bottom: 10px; }
  </style>
</head><body>
  <div class="header"><h1>Remote-Door V1.0 (Ethernet)</h1></div>
  <div class="container">
    <h3>Налаштування пристрою</h3>
    <form action="/save" method="POST">
      <label>Системний пароль (для AP та скидання)</label><input type="text" name="ap_pass" placeholder="Мінімум 8 символів">
      <label>Telegram Bot Token</label><input type="text" name="token" placeholder="Токен бота">
      <label>Власник (Chat ID)</label><input type="text" name="owner" placeholder="ID власника">
      <label>Група (Group ID)</label><input type="text" name="group" placeholder="ID групи (необов'язково)">
      <input type="submit" value="ЗБЕРЕГТИ">
    </form>
    <div class="footer">
      <h4>Developed by Stanislav Turii</h4>
      <a href='https://github.com/Stanislav-developer'>GitHub Repository</a>
    </div>
  </div>
</body></html>)rawliteral";

// Функція відображення головної сторінки сервера
void handleRoot() {
  server.send(200, "text/html; charset=utf-8", index_html);
}

// Функція валідації та збереження отриманих даних у Preferences
void handleSave() {
  if (server.hasArg("token") && server.hasArg("owner") && server.hasArg("ap_pass")) {
    String n_token = server.arg("token");
    String n_owner = server.arg("owner");
    String n_group = server.arg("group");
    String n_ap_pass = server.arg("ap_pass");

    if (n_ap_pass.length() >= 8) {
      preferences.putString("token", n_token);
      preferences.putString("chatId", n_owner);
      preferences.putString("groupId", n_group);
      preferences.putString("apPass", n_ap_pass);

      String response = "<html><head><meta charset='utf-8'></head><body style='background-color:#0b132b; color:white; text-align:center; font-family:Arial;'>";
      response += "<h1 style='color:#5bc0be;'>Збережено! ✅</h1><p>Перезавантаження...</p></body></html>";
      server.send(200, "text/html; charset=utf-8", response);
      
      delay(2000);
      ESP.restart(); // Перезапуск для застосування конфігурації
    } else {
      server.send(400, "text/plain; charset=utf-8", "Помилка: Системний пароль має бути мінімум 8 символів!");
      return;
    }
  } else {
    server.send(400, "text/plain; charset=utf-8", "Error: Missing data");
  }
}

// Функція ініціалізації аварійної точки доступу та вічного циклу Веб-сервера
void launchWebServer() {
    WiFi.mode(WIFI_AP);
    if (apPassword.length() >= 8 && apPassword != "1") {
      WiFi.softAP("Remote-Door-Setup", apPassword.c_str());
    } else {
      WiFi.softAP("Remote-Door-Setup");
    }
    
    dnsServer.start(53, "*", WiFi.softAPIP());
    server.on("/", handleRoot);
    server.on("/save", handleSave);
    server.onNotFound(handleRoot);
    server.begin();
    
    Serial.println("[DEBUG] Точка доступу створена. IP: " + WiFi.softAPIP().toString());

    int fadeValue = 0;
    int fadeDirection = 5;
    unsigned long lastFadeTime = 0;

    // Вічний ізольований цикл обробки веб-запитів із плавним ШІМ ефектом для системного LED
    while (true) {
      dnsServer.processNextRequest();
      server.handleClient();
      
      if (millis() - lastFadeTime > 25) {
        lastFadeTime = millis();
        fadeValue += fadeDirection;
        if (fadeValue <= 0 || fadeValue >= 255) {
          fadeDirection = -fadeDirection; 
        }
        ledcWrite(LED_SYS_PIN, fadeValue);
      }
    }
}

// Конвертація лічильника секунд аптайму у текстовий формат (дні, години, хвилини)
String formatDuration(unsigned long seconds) {
  unsigned long days = seconds / 86400;
  unsigned long hours = (seconds % 86400) / 3600;
  unsigned long minutes = (seconds % 3600) / 60;
  unsigned long secs = seconds % 60;
  String result = "";
  if (days > 0) result += String(days) + " д. ";
  if (hours > 0 || days > 0) result += String(hours) + " год. ";
  if (minutes > 0 || hours > 0 || days > 0) result += String(minutes) + " хв. ";
  result += String(secs) + " сек.";
  return result;
}

// Функція генерації низькорівневого імпульсу на реле для відкриття замка
void openDoor() {
  digitalWrite(RELAY_PIN, LOW);   // Гарантуємо низький рівень (увімкнення реле)
  pinMode(RELAY_PIN, OUTPUT);     // Перемикаємо на вихід для подачі землі на реле
  delay(1000);                    // Утримання імпульсу 1 секунду (стандарт контролерів)
  pinMode(RELAY_PIN, INPUT);      // Переводимо у високоімпедансний стан (відпускаємо реле)
  
  openCount++;
  preferences.putInt("openCount", openCount);
  Serial.println("[DEBUG] Двері фізично відчинено (імпульс реле завершено).");
}

// Функція розбору та маршрутизації команд, що прийшли через мережу з серверів Telegram
void handleNewMessages() {
  String chat_id = String(bot.messages[0].chat_id);
  String text = bot.messages[0].text;
  String from_name = bot.messages[0].from_name;
  
  if (chat_id == chatId || chat_id == groupId) {
    
    // КРИТИЧНИЙ ЗАХИСТ: Ігноруємо порожні текстові об'єкти (медіафайли, стікери), запобігаючи паніці ядра
    if (text.length() == 0) return;

    // Очищення та нормалізація синтаксису команд (відрізання аргументів та тегів бота)
    String cmd = text;
    int spaceIdx = cmd.indexOf(' ');
    if (spaceIdx != -1) cmd = cmd.substring(0, spaceIdx); 
    int atIdx = cmd.indexOf('@');
    if (atIdx != -1) cmd = cmd.substring(0, atIdx);       

    // Обробка введення пароля для підтвердження процедури повного очищення флеш-пам'яті
    if (waitingForClearDataPass && chat_id == clearDataRequesterId) {
        if (text == apPassword && apPassword.length() > 0) {
            Serial.println("[DEBUG] Користувач [" + from_name + "] ввів ПРАВИЛЬНИЙ пароль. Очищення пам'яті NVS та рестарт.");
            preferences.clear();
            preferences.putString("token", "1");
            preferences.putString("chatId", "1");
            preferences.putString("groupId", "1");
            preferences.putString("apPass", "1");
            
            bot.sendMessage(chat_id, "⚠️ Дані очищені. Перезапуск у режим конфігурації...", "");
            delay(1000);
            ESP.restart(); 
        } else {
            Serial.println("[DEBUG] Користувач [" + from_name + "] ввів НЕВІРНИЙ пароль для очищення даних. Скасовано.");
            bot.sendMessage(chat_id, "❌ Невірний пароль. Операцію скасовано.", "");
            waitingForClearDataPass = false;
        }
        return; 
    }

    // Команда дистанційного відкриття дверей
    if (cmd == "/unlock") {
      if (isButtonCooldown) {
        unsigned long remaining = (COOLDOWN_DURATION - (millis() - cooldownStartTime)) / 1000;
        if (remaining == 0) remaining = 1;
        
        Serial.println("[DEBUG] Користувач [" + from_name + "] викликав /unlock під час таймауту. ВІДХИЛЕНО. Залишилось: " + String(remaining) + "с.");
        bot.sendMessage(chat_id, "⏳ <b>Зачекайте!</b> Замок щойно відчинявся. Тайм-аут: " + String(remaining) + " сек.", "HTML");
      } else {
        Serial.println("[DEBUG] Користувач [" + from_name + "] викликав /unlock. Виконую відкриття дверей...");
        bot.sendMessage(chat_id, "🔓 Користувач <b>" + from_name + "</b> відчинив двері.", "HTML");
        
        openDoor(); 
        
        isButtonCooldown = true;
        cooldownStartTime = millis(); // Старт таймауту безпеки строго після закриття реле
      }
    }
    
    // Команда активації локальної кнопки виходу
    else if (cmd == "/allow") {
      Serial.println("[DEBUG] Користувач [" + from_name + "] надіслав /allow. Доступ з кнопки дозволено.");
      isAccessAllowed = true;
      if (!isButtonCooldown) digitalWrite(LED_BTN_PIN, HIGH); 
      bot.sendMessage(chat_id, "✅ Користувач <b>" + from_name + "</b> ДОЗВОЛИВ відкриття дверей з кнопки.", "HTML");
    }
    
    // Команда повної деактивації локальної кнопки виходу
    else if (cmd == "/deny") {
      Serial.println("[DEBUG] Користувач [" + from_name + "] надіслав /deny. Доступ з кнопки ЗАБОРОНЕНО.");
      isAccessAllowed = false;
      digitalWrite(LED_BTN_PIN, LOW); 
      bot.sendMessage(chat_id, "🚫 Користувач <b>" + from_name + "</b> ЗАБОРОНИВ відкриття дверей з кнопки.", "HTML");
    }
    
    // Команда телеметрії та діагностики аптайму системи
    else if (cmd == "/status") {
      Serial.println("[DEBUG] Користувач [" + from_name + "] запитав статус системи за командою /status.");
      String status_message = "📊 <b>Статус Remote-Door:</b>\n\n";
      status_message += "Стан доступу: ";
      status_message += isAccessAllowed ? "🟢 Дозволено\n" : "🔴 Заборонено\n";
      status_message += "Кількість відкривань: <b>" + String(openCount) + "</b>\n";
      unsigned long uptime = millis() / 1000;
      status_message += "Пристрій працює: " + formatDuration(uptime);
      bot.sendMessage(chat_id, status_message, "HTML");
    }
    
    // Запит на повне скидання конфігураційних параметрів контролера
    else if (cmd == "/clear_data") {
      Serial.println("[DEBUG] Користувач [" + from_name + "] ініціював команду /clear_data. Очікування підтвердження паролем.");
      waitingForClearDataPass = true;
      clearDataRequesterId = chat_id;
      bot.sendMessage(chat_id, "⚠️ <b>УВАГА!</b> Ви дійсно бажаєте стерти всі дані та статистику?\n\nНадішліть ваш <b>системний пароль</b> для підтвердження або будь-яний інший текст для скасування.", "HTML");
    }
    
    // Команди виведення інформаційного меню довідки
    else if (cmd == "/help" || cmd == "/start") {
      Serial.println("[DEBUG] Користувач [" + from_name + "] викликав довідку за командою " + cmd);
      String help_message = "👋 Привіт, " + from_name + "!\n\n";
      help_message += "Доступні команди:\n";
      help_message += "/unlock - Відкрити двері\n";
      help_message += "/allow - Дозволити доступ з кнопки\n";
      help_message += "/deny - Заборонити доступ з кнопки\n";
      help_message += "/status - Стан системи\n";
      help_message += "/clear_data - Скидання до заводських\n";
      bot.sendMessage(chat_id, help_message, "");
    }
  }
}

// Перехоплювач системних подій Ethernet драйвера ядра (lwIP стек)
void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("[DEBUG] Нативний драйвер ETH запущено.");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("[DEBUG] Кабель Ethernet підключено (Фізичний лінк OK) ✅");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.print("[DEBUG] Роутер видав IP адресу: ");
      Serial.println(ETH.localIP());
      eth_connected = true;
      sysLedInterval = 0; // Постійне стабільне світіння — мережа наявна
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("[WARNING] Кабель Ethernet відключено!");
      eth_connected = false;
      sysLedInterval = 300; // Повертаємо режим швидкого блимання (пошук мережі)
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("[DEBUG] Драйвер ETH зупинено.");
      eth_connected = false;
      break;
    default:
      break;
  }
}

// Функція первинної апаратної ініціалізації периферії (виконується одноразово)
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n========================================");
  Serial.println("[DEBUG] Запуск системи Remote-Door V1.0 (Native ETH)");
  Serial.println("========================================");

  // Налаштування режимів роботи GPIO виводів
  pinMode(RELAY_PIN, INPUT); // За замовчуванням Z-стан (безпека від випадкових спрацювань замка)
  pinMode(LED_BTN_PIN, OUTPUT);
  digitalWrite(LED_BTN_PIN, LOW); 
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(9, INPUT_PULLUP); // Кнопка Boot для виходу в режим Веб-сервера вручну

  // Конфігурація апаратних таймерів ШІМ двигуна (LEDC драйвер) під нове ядро Core v3.x
  ledcAttach(LED_SYS_PIN, 1000, 8); // Частота 1 кГц, розрядність 8 біт (0-255)
  ledcWrite(LED_SYS_PIN, 0); 

  preferences.begin("remote-door", false); // Відкриваємо простір імен preferences

  // Аварійний запуск конфігуратора при апаратному утриманні кнопки BOOT при старті
  if (digitalRead(9) == LOW) {
    launchWebServer();
  }

  // Читання параметрів конфігурації з енергонезалежної пам'яті
  botToken = preferences.getString("token", botToken);
  chatId = preferences.getString("chatId", chatId);
  groupId = preferences.getString("groupId", groupId);
  apPassword = preferences.getString("apPass", apPassword);
  openCount = preferences.getInt("openCount", 0);

  // Перевірка наявності валідних даних; якщо флеш чистий — запуск конфігуратора
  if (botToken == "1" || botToken == "") {
    launchWebServer();
  }

  WiFi.onEvent(WiFiEvent); // Реєстрація ядра для відстеження подій кабелю

  // Апаратний цикл скидання Ethernet контролера W5500 для стабілізації заліза
  Serial.println("[DEBUG] Виконую апаратне скидання W5500...");
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);
  delay(100);
  digitalWrite(W5500_RST, HIGH);
  delay(500); 

  // Ініціалізація шини SPI (передаємо -1 для CS, оскільки ним автоматично керує ETH.h)
  SPI.begin(W5500_SCK, W5500_MISO, W5500_MOSI, -1);

  // Інтеграція інтерфейсу W5500 безпосередньо у внутрішній стек lwIP ядра операційної системи
  Serial.println("[DEBUG] Монтування чипа W5500 у систему...");
  if (!ETH.begin(ETH_PHY_W5500, 1, W5500_CS, -1, W5500_RST, SPI)) {
    Serial.println("[ERROR] Помилка монтування W5500!");
    sysLedInterval = 300;
  }

  // Network initialization connection timeout safety loop
  unsigned long startOrder = millis();
  while (!eth_connected && millis() - startOrder < 15000) {
    if (millis() - lastSysLedBlink > 300) {
      lastSysLedBlink = millis();
      sysLedState = !sysLedState;
      ledcWrite(LED_SYS_PIN, sysLedState ? 255 : 0);
    }
    delay(10);
  }

  // Конфігурація та оптимізація таймаутів нативного клієнта TLS (mbedtls)
  client.setTimeout(3000); // Запобігає тривалому блокуванню сторожового таймера (Watchdog)
  client.setInsecure();    // Використовуємо спрощене SSL рукостискання (без локального парсингу сертифікатів)
  bot.updateToken(botToken);

  // Ініціалізація та синхронізація системного годинника через мережеві сервери часу
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  if (eth_connected) {
    String startMessage = "🔐 <b>Контролер Remote-Door активний!</b>\n";
    startMessage += "Версія: V1.0 | Підключено успішно.";
    
    Serial.println("[DEBUG] Спроба відправити повідомлення через нативний стек...");
    if (bot.sendMessage(chatId, startMessage, "HTML")) {
      Serial.println("[DEBUG] Повідомлення надіслано успішно! ✅");
      sysLedInterval = 0; 
    } else {
      Serial.println("[ERROR] Помилка відправки. Перехід у Web-Setup.");
      sysLedInterval = 1500; 
      delay(2000); 
      launchWebServer(); 
    }
  }

  // Очищення черги старих запитів для запобігання хибних спрацювань замка після увімкнення
  int newMessage = bot.getUpdates(-1);
  if (newMessage > 0) {
    bot.last_message_received = bot.messages[0].update_id;
  }
}

// Головний нескінченний цикл виконання програми мікроконтролера
void loop() {
  // Асинхронний диспетчер режимів індикації системного світлодіода
  if (sysLedInterval > 0) {
    if (millis() - lastSysLedBlink > sysLedInterval) {
      lastSysLedBlink = millis();
      sysLedState = !sysLedState;
      ledcWrite(LED_SYS_PIN, sysLedState ? 255 : 0);
    }
  } else {
    ledcWrite(LED_SYS_PIN, 255); 
  }

  // Сканування стану фізичної стінової кнопки виходу (блокується під час дії таймауту безпеки)
  if (isAccessAllowed && !isButtonCooldown && digitalRead(BUTTON_PIN) == LOW) {
    delay(50); // Апаратне вікно фільтрації брязкоту контактів кнопки
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("[DEBUG] Натиснуто фізичну кнопку відкриття дверей.");
      
      openDoor(); // Виклик функції імпульсу реле. Кнопка почне миготіти тільки після закриття замка
      
      isButtonCooldown = true;
      cooldownStartTime = millis();
      while(digitalRead(BUTTON_PIN) == LOW) { delay(10); } // Очікуємо повного відпускання кнопки користувачем
    }
  }

  // Алгоритм генерації миготіння підсвітки кнопки під час діючого блокування (Cooldown)
  if (isButtonCooldown) {
    if (millis() - cooldownStartTime < COOLDOWN_DURATION) {
      if (millis() - lastBtnLedToggle > 250) {
        lastBtnLedToggle = millis();
        btnLedState = !btnLedState;
        digitalWrite(LED_BTN_PIN, btnLedState);
      }
    } else {
      isButtonCooldown = false;
      Serial.println("[DEBUG] Тайм-аут пристрою завершено. Замок та кнопка знову доступні.");
      if (isAccessAllowed) {
        digitalWrite(LED_BTN_PIN, HIGH); 
      } else {
        digitalWrite(LED_BTN_PIN, LOW);  
      }
    }
  }

  // Опитування та читання нових повідомлень Telegram (активне тільки при наявності лінку Ethernet)
  if (eth_connected) {
    static unsigned long lastBotCheck = 0;
    if (millis() - lastBotCheck > 1000) { // Перевірка виконується суворо один раз на секунду (без затримок)
      int newMessage = bot.getUpdates(-1);
      if (newMessage) {
        handleNewMessages();
      }
      lastBotCheck = millis();
    }
  }
}
