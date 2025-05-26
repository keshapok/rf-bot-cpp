#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cmath>
#include <windows.h>
#include <opencv2/opencv.hpp>

using namespace std;
using namespace cv;
using namespace std::chrono;

// ================== Настройки ==================
const int MIN_MOB_SIZE = 500;
const int SCAN_RADIUS = 300;
const int MOTION_THRESHOLD = 25;
const Rect MINIMAP_RECT(860, 10, 110, 110); // Примерные координаты миникарты

bool bot_active = false;
bool running = true;

// ================== Функции ==================

// Получаем хендл окна игры
HWND get_game_window(const string& window_title) {
    return FindWindowA(NULL, window_title.c_str());
}

// Захват области экрана
Mat capture_screen(HWND hwnd) {
    RECT rect;
    if (!hwnd || !GetClientRect(hwnd, &rect)) {
        cout << "Ошибка: Не найдено окно игры" << endl;
        return Mat();
    }

    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    HDC hdc_screen = GetDC(NULL);
    HDC hdc_mem = CreateCompatibleDC(hdc_screen);
    HBITMAP hbitmap = CreateCompatibleBitmap(hdc_screen, width, height);
    HGDIOBJ old_obj = SelectObject(hdc_mem, hbitmap);

    BitBlt(hdc_mem, 0, 0, width, height, hdc_screen, 0, 0, SRCCOPY);

    BITMAPINFOHEADER bi;
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = -height;  // Верх вниз головой
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_BITFIELDS;
    bi.biSizeImage = 0;

    DWORD rmask[3] = { 0xFF0000, 0xFF00, 0xFF };
    memcpy(bi.biBitFields, rmask, 3 * sizeof(DWORD));

    Mat result(height, width, CV_8UC4);
    GetDIBits(hdc_mem, hbitmap, 0, height, result.data, (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    SelectObject(hdc_mem, old_obj);
    DeleteObject(hbitmap);
    DeleteDC(hdc_mem);
    ReleaseDC(NULL, hdc_screen);

    cvtColor(result, result, COLOR_BGRA2BGR);
    return result;
}

// Обнаружение мобов
vector<Rect> detect_mobs(Mat frame, Mat& prev_gray) {
    Mat gray;
    cvtColor(frame, gray, COLOR_BGR2GRAY);
    GaussianBlur(gray, gray, Size(11, 11), 0);

    vector<Rect> mobs;
    if (prev_gray.empty()) {
        prev_gray = gray.clone();
        return mobs;
    }

    if (gray.size() != prev_gray.size()) {
        prev_gray = gray.clone();
        return mobs;
    }

    Mat delta, thresh;
    absdiff(prev_gray, gray, delta);
    threshold(delta, thresh, MOTION_THRESHOLD, 255, THRESH_BINARY);
    dilate(thresh, thresh, Mat());

    vector<vector<Point>> contours;
    findContours(thresh, contours, RETR_EXTERNAL, CHAIN_APPROX_SIMPLE);

    Point center(frame.cols / 2, frame.rows / 2);
    for (auto& cnt : contours) {
        double area = contourArea(cnt);
        if (area < MIN_MOB_SIZE) continue;

        Rect r = boundingRect(cnt);
        Point mob_center(r.x + r.width / 2, r.y + r.height / 2);
        double dist = norm(mob_center - center);
        if (dist <= SCAN_RADIUS) {
            mobs.push_back(r);
        }
    }

    gray.copyTo(prev_gray);
    return mobs;
}

// Атака на моба
void attack_mob(Rect mob, HWND hwnd) {
    RECT game_rect;
    GetClientRect(hwnd, &game_rect);

    int target_x = mob.x + mob.width / 2 + game_rect.left;
    int target_y = mob.y + mob.height / 2 + game_rect.top;

    SetCursorPos(target_x, target_y);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);

    keybd_event(VK_SPACE, 0, 0, 0);     // Нажать Space
    keybd_event(VK_SPACE, 0, KEYEVENTF_KEYUP, 0); // Отпустить
    cout << "Атака на координаты: (" << target_x << ", " << target_y << ")" << endl;
}

// Проверка поворота камеры по миникарте
bool check_camera_rotation(Mat frame) {
    static Mat last_minimap;
    static bool first_run = true;

    Mat minimap = frame(MINIMAP_RECT);
    Mat gray_minimap;
    cvtColor(minimap, gray_minimap, COLOR_BGR2GRAY);

    vector<vector<Point>> contours;
    Mat thresh;
    threshold(gray_minimap, thresh, 127, 255, THRESH_BINARY);
    findContours(thresh, contours, RETR_EXTERNAL, CHAIN_APPROX_SIMPLE);

    if (contours.empty()) return false;

    Moments m = moments(contours[0]);
    if (m.m00 == 0) return false;

    Point current_dir(m.m10 / m.m00, m.m01 / m.m00);

    if (first_run) {
        last_minimap = current_dir;
        first_run = false;
        return false;
    }

    double dx = current_dir.x - last_minimap.x;
    double dy = current_dir.y - last_minimap.y;
    double distance = sqrt(dx * dx + dy * dy);

    last_minimap = current_dir;

    if (distance > 20) {
        cout << "Вращение камеры превышает допустимый предел. Игнорируем." << endl;
        return false;
    }

    return true;
}

// Горячая клавиша F12
DWORD WINAPI hotkey_listener(LPVOID lpParam) {
    while (running) {
        if (GetAsyncKeyState(VK_F12) & 0x8000) {
            bot_active = !bot_active;
            cout << "[F12] Бот: " << (bot_active ? "Активен" : "Неактивен") << endl;
            Sleep(500);
        }
        Sleep(100);
    }
    return 0;
}

// ================== Основной цикл ==================
int main() {
    HWND hwnd = get_game_window("[PREMIUM] RF Online");
    if (!hwnd) {
        cerr << "Ошибка: Не найдено окно игры" << endl;
        system("pause");
        return 1;
    }

    Mat prev_gray;
    Mat frame;
    thread hotkey_thread(hotkey_listener, NULL);

    cout << "Бот запущен. Нажмите F12, чтобы начать/остановить. Q — выход." << endl;

    while (running) {
        frame = capture_screen(hwnd);
        if (frame.empty()) {
            this_thread::sleep_for(milliseconds(100));
            continue;
        }

        check_camera_rotation(frame);
        vector<Rect> mobs = detect_mobs(frame, prev_gray);

        if (bot_active && !mobs.empty()) {
            attack_mob(mobs[0], hwnd);
        }

        imshow("RF Bot", frame);
        char key = waitKey(1);
        if (key == 'q') break;

        this_thread::sleep_for(milliseconds(700)); // Интервал между проверками
    }

    running = false;
    hotkey_thread.join();

    cout << "Программа завершена." << endl;
    destroyAllWindows();
    return 0;
}
