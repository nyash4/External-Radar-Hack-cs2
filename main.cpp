#include <windows.h> // кфг для остальных карт
#include <gdiplus.h>
#include <iostream>
#include "offsets.h"
#include <string>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <regex>
#include "memory.h"

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;
using namespace std;

static float g_scaleFactor = 1.0f;
static float g_offsetX = 0.0f;
static float g_offsetY = 0.0f;
static int g_windowWidth = 800;
static int g_windowHeight = 600;

// Глобальные переменные для карты
static int g_imgWidth = 0, g_imgHeight = 0;
Bitmap* g_mapImage = nullptr;
std::string g_CurrentMapName = "";

// Глобальные переменные для границ карты (значения по умолчанию)
double g_worldXMin = -3230, g_worldXMax = 3230, g_worldYMin = -3230, g_worldYMax = 3230;

// Структура для хранения координат экрана
struct Vec2 {
    double x, y;
};

// Структура для хранения данных о сущности с информацией о команде
struct EntityData {
    Vec2 position;
    bool isValid;
    int team;
};

// Вектор для хранения данных о сущностях (индексы 1..63, индекс 0 не используется)
vector<EntityData> entities(256, { {0, 0}, false, 0 });

// Глобальная переменная для локальной команды
int g_LocalTeam = 0;
int g_localPlayerIndex = 0;

// Потоковая переменная для завершения фонового потока
volatile bool g_Running = true;

// Критическая секция для синхронизации доступа к данным сущностей
CRITICAL_SECTION g_entitiesCS;

// Дескриптор фонового потока обновления сущностей
HANDLE g_EntityThread = NULL;

// Функция преобразования мировых координат в координаты изображения
Vec2 worldToScreen(double X, double Y,
    double x_min, double x_max,
    double y_min, double y_max,
    int imgWidth, int imgHeight) {
    Vec2 screen;
    // Пример расчёта (если координаты карты могут быть отрицательными, используется суммарный диапазон)
    screen.x = ((X - x_min) * imgWidth) / (x_max - x_min);
    screen.y = ((y_max - Y) * imgHeight) / (y_max - y_min);
    return screen;
}

// Функция загрузки конфигурации карты из файла .cfg
void LoadMapConfig(const std::string& currentMap) {
    ifstream cfgFile("mapCfg.cfg");  // Открываем файл конфигурации
    if (!cfgFile.is_open()) {
        cout << "[DEBUG] Не удалось открыть mapCfg.cfg. Используем значения по умолчанию." << endl;
        return;
    }

    string line;
    while (getline(cfgFile, line)) {
        // Убираем пробелы с начала и конца строки
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);

        // Ищем нужную карту по имени
        if (line.find(currentMap) != string::npos) {
            // Используем регулярное выражение для извлечения блока параметров
            regex rgx("\"" + currentMap + "\"\\{([^}]*)\\}");
            smatch match;
            if (regex_search(line, match, rgx)) {
                string params = match[1].str();
                // Ищем все переменные с числами (например, pos_xMin=-3230)
                regex numberPattern("([a-zA-Z_]+)=([-+]?[0-9]*\\.?[0-9]+)");
                smatch numberMatch;
                while (regex_search(params, numberMatch, numberPattern)) {
                    string key = numberMatch[1].str();
                    double value = stod(numberMatch[2].str());
                    if (key == "pos_xMin") g_worldXMin = value;
                    else if (key == "pos_xMax") g_worldXMax = value;
                    else if (key == "pos_yMin") g_worldYMin = value;
                    else if (key == "pos_yMax") g_worldYMax = value;
                    params = numberMatch.suffix().str();
                }
                cout << "[DEBUG] Загружена конфигурация для карты " << currentMap
                    << ": pos_xMin = " << g_worldXMin << ", pos_xMax = " << g_worldXMax
                    << ", pos_yMin = " << g_worldYMin << ", pos_yMax = " << g_worldYMax << endl;
                cfgFile.close();
                return;
            }
        }
    }
    cout << "[DEBUG] Конфигурация для карты " << currentMap << " не найдена. Используем значения по умолчанию." << endl;
    cfgFile.close();
}

// Функция, выполняемая в отдельном потоке для обновления данных сущностей
DWORD WINAPI EntityUpdateThread(LPVOID lpParam) {
    while (g_Running) {
        // Чтение локального игрока и обновление номера команды с обработкой исключений
        uintptr_t localPlayer = 0;
        __try {
            localPlayer = VARS::memRead<uintptr_t>(VARS::baseAddress + offsets::dwLoclalPlayerPawn);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            localPlayer = 0;
        }
        if (localPlayer) {
            __try {
                g_LocalTeam = VARS::memRead<int>(localPlayer + offsets::m_iTeamNum);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                g_LocalTeam = 0;
            }
        }

        // Обход сущностей: используем корректный диапазон и шаг смещения (0x10)
        for (int i = 0; i < static_cast<int>(entities.size()); i++) {
            uintptr_t ent = 0;
            __try {
                ent = VARS::memRead<uintptr_t>(VARS::baseAddress + offsets::dwEntityList + (0x01 * i));
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                ent = 0;
            }

            if (ent == localPlayer) {
                g_localPlayerIndex = i;
            }

            if (ent == 0 || ent == static_cast<uintptr_t>(-1)) {
                EnterCriticalSection(&g_entitiesCS);
                entities[i].isValid = false;
                LeaveCriticalSection(&g_entitiesCS);
                continue;
            }

            int HP = 0;
            __try {
                HP = VARS::memRead<int>(ent + offsets::m_iHealth);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                HP = 0;
            }
            if (HP <= 0 || HP > 100) {
                EnterCriticalSection(&g_entitiesCS);
                entities[i].isValid = false;
                LeaveCriticalSection(&g_entitiesCS);
                continue;
            }

            int entTeam = 0;
            __try {
                entTeam = VARS::memRead<int>(ent + offsets::m_iTeamNum);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                entTeam = 0;
            }

            float PosX = 0, PosY = 0;
            __try {
                PosX = VARS::memRead<float>(ent + offsets::XPos);
                PosY = VARS::memRead<float>(ent + offsets::YPos);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                PosX = 0;
                PosY = 0;
            }

            Vec2 screen = worldToScreen(PosX, PosY, g_worldXMin, g_worldXMax, g_worldYMin, g_worldYMax, g_imgWidth, g_imgHeight);

            EnterCriticalSection(&g_entitiesCS);
            entities[i].position = screen;
            entities[i].isValid = true;
            entities[i].team = entTeam;
            LeaveCriticalSection(&g_entitiesCS);
        }
        Sleep(30);
    }
    return 0;
}

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void DrawFrame(HDC hdc, Graphics& graphics, Bitmap& image, int imgWidth, int imgHeight);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Инициализация GDI+
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    // Регистрация класса окна
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"RadarHack";
    RegisterClass(&wc);

    // Создание окна
    HWND hwnd = CreateWindow(
        wc.lpszClassName, L"RadarHack", WS_OVERLAPPEDWINDOW,
        100, 100, 800, 600, NULL, NULL, hInstance, NULL
    );
    ShowWindow(hwnd, nCmdShow);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    try {
        switch (message) {
        case WM_CREATE: {
            // Чтение имени карты из памяти
            uintptr_t CurrentMapPtr = VARS::memRead<uintptr_t>(VARS::baseAddress + offsets::CurrentMap);
            std::string CurrentMapStr = VARS::memReadString(CurrentMapPtr, 11);
            // Сохраняем текущее имя карты в глобальную переменную
            g_CurrentMapName = CurrentMapStr;

            // Загрузка конфигурации карты
            LoadMapConfig(CurrentMapStr);

            // Формирование пути к файлу карты
            std::string MapPath = "RadarPng/" + CurrentMapStr + "_radar.png";
            std::wstring WMapPath(MapPath.begin(), MapPath.end());

            // Загрузка изображения карты
            g_mapImage = new Bitmap(WMapPath.c_str());
            if (!g_mapImage || g_mapImage->GetLastStatus() != Ok) {
                cout << "Не удалось загрузить изображение карты!" << endl;
                PostQuitMessage(0);
            }

            g_imgWidth = g_mapImage->GetWidth();
            g_imgHeight = g_mapImage->GetHeight();

            // Установка размеров окна с учётом рамки
            RECT rc = { 0, 0, g_imgWidth, g_imgHeight };
            AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOMOVE);

            // Инициализируем критическую секцию
            InitializeCriticalSection(&g_entitiesCS);

            // Создаем поток для обновления данных сущностей
            g_Running = true;
            g_EntityThread = CreateThread(NULL, 0, EntityUpdateThread, NULL, 0, NULL);

            // Установка таймера для обновления отрисовки и проверки смены карты
            SetTimer(hwnd, 1, 30, NULL);
            break;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // Создаем совместимый DC для двойной буферизации
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, g_windowWidth, g_windowHeight);
            HGDIOBJ oldBitmap = SelectObject(memDC, memBitmap);

            // Создаем объект Graphics для memDC
            Graphics graphics(memDC);
            graphics.SetSmoothingMode(SmoothingModeAntiAlias);

            // Заполняем фон (можно заменить на свой цвет или градиент)
            SolidBrush bgBrush(Color(30, 30, 30));
            graphics.FillRectangle(&bgBrush, 0, 0, g_windowWidth, g_windowHeight);

            // Отрисовка карты и объектов
            if (g_mapImage) {
                DrawFrame(memDC, graphics, *g_mapImage, g_imgWidth, g_imgHeight);
            }

            // Перенос буфера на экран
            BitBlt(hdc, 0, 0, g_windowWidth, g_windowHeight, memDC, 0, 0, SRCCOPY);

            // Очистка ресурсов
            SelectObject(memDC, oldBitmap);
            DeleteObject(memBitmap);
            DeleteDC(memDC);

            EndPaint(hwnd, &ps);
            break;
        }

        case WM_SIZE: {
            g_windowWidth = LOWORD(lParam);
            g_windowHeight = HIWORD(lParam);

            // Рассчет масштаба с сохранением пропорций
            float widthRatio = static_cast<float>(g_windowWidth) / g_imgWidth;
            float heightRatio = static_cast<float>(g_windowHeight) / g_imgHeight;
            g_scaleFactor = min(widthRatio, heightRatio);

            // Рассчет смещения для центрирования
            g_offsetX = (g_windowWidth - (g_imgWidth * g_scaleFactor)) / 2.0f;
            g_offsetY = (g_windowHeight - (g_imgHeight * g_scaleFactor)) / 2.0f;

            InvalidateRect(hwnd, NULL, TRUE);
            break;
        }

        case WM_TIMER: {
            // Проверка, изменилась ли карта
            uintptr_t currentMapPtr = 0;
            std::string newMapStr;
            
                currentMapPtr = VARS::memRead<uintptr_t>(VARS::baseAddress + offsets::CurrentMap);
                newMapStr = VARS::memReadString(currentMapPtr, 11);
           

            if (newMapStr != g_CurrentMapName) {
                // Обновляем глобальное имя карты, конфигурацию и изображение
                g_CurrentMapName = newMapStr;
                LoadMapConfig(newMapStr);
                std::string MapPath = "RadarPng/" + newMapStr + "_radar.png";
                std::wstring WMapPath(MapPath.begin(), MapPath.end());
                if (g_mapImage) {
                    delete g_mapImage;
                    g_mapImage = nullptr;
                }
                g_mapImage = new Bitmap(WMapPath.c_str());
                if (!g_mapImage || g_mapImage->GetLastStatus() != Ok) {
                    cout << "Не удалось загрузить изображение карты " << newMapStr << "!" << endl;
                }
                else {
                    g_imgWidth = g_mapImage->GetWidth();
                    g_imgHeight = g_mapImage->GetHeight();
                    RECT rc = { 0, 0, g_imgWidth, g_imgHeight };
                    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
                    SetWindowPos(hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOMOVE);
                }
            }
            // Перерисовка окна (точек) без стирания фона
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }

        case WM_DESTROY:
            // Завершаем фоновый поток обновления
            g_Running = false;
            WaitForSingleObject(g_EntityThread, INFINITE);
            CloseHandle(g_EntityThread);
            DeleteCriticalSection(&g_entitiesCS);

            KillTimer(hwnd, 1);
            if (g_mapImage) {
                delete g_mapImage;
                g_mapImage = nullptr;
            }
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProc(hwnd, message, wParam, lParam);
        }
    }
    catch (const std::exception& e) { 
        MessageBox(hwnd, (LPCWSTR)e.what(), L"Error", MB_OK | MB_ICONERROR);
    }
    return 0;
}

// Функция отрисовки карты и точек сущностей
void DrawFrame(HDC hdc, Graphics& graphics, Bitmap& image, int imgWidth, int imgHeight) {
    // Отрисовка масштабированной карты
    RectF destRect(g_offsetX, g_offsetY, imgWidth * g_scaleFactor, imgHeight * g_scaleFactor);
    graphics.DrawImage(&image, destRect, 0, 0, imgWidth, imgHeight, UnitPixel);

    // Блокируем доступ к вектору сущностей
    EnterCriticalSection(&g_entitiesCS);
    for (int i = 0; i < static_cast<int>(entities.size()); i++) {
        if (entities[i].isValid) {
            Vec2 screen = entities[i].position;

            // Применяем масштаб и смещение
            float scaledX = static_cast<float>(screen.x * g_scaleFactor + g_offsetX);
            float scaledY = static_cast<float>(screen.y * g_scaleFactor + g_offsetY);

            Color color;
            if (i == g_localPlayerIndex) {
                color = Color(255, 255, 0); // Желтый для localPlayer
            }
            else {
                color = (entities[i].team == g_LocalTeam) ? Color(0, 255, 0) : Color(255, 0, 0);
            }

            SolidBrush brush(color);
            graphics.FillEllipse(
                &brush,
                static_cast<REAL>(scaledX ),
                static_cast<REAL>(scaledY ),
                static_cast<REAL>(10),
                static_cast<REAL>(10)
            );
        }
    }
    LeaveCriticalSection(&g_entitiesCS);
}
