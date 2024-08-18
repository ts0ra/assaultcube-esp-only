#include "memory.h"
#include "offsets.h"
#include "enums.h"

#include <Windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>
#include <thread>
#include <sstream>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dx9.lib")

// External declaration for ImGui's Win32 WndProc handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Function declarations
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void initD3D(HWND hwnd);
void cleanD3D();
void UpdateOverlayPosition(HWND targetWindow, HWND overlayWindow);
void HandleOverlayVisibility(HWND targetWindow, HWND overlayWindow);
bool WorldToScreen(const D3DXVECTOR3& pos, D3DXVECTOR3& screen, const D3DMATRIX& matrix, int width, int height);
bool isTeamGameMode(int gameMode);
bool isItemPickup(int itemType);
bool isItemRedFlag(int itemType, int itemAttr2);
bool isItemBlueFlag(int itemType, int itemAttr2);
void GetItemInfo(int itemAttr2, int itemType, char(&itemName)[16], ImColor& color);

// Struct
struct Vector3ItemEntity
{
    short x, y, z;

    D3DXVECTOR3 ToD3DXVECTOR3() const
    {
        return D3DXVECTOR3(static_cast<float>(x),
            static_cast<float>(y),
            static_cast<float>(z));
    }
};

// Global variables
LPDIRECT3D9 d3d = NULL;             // Direct3D interface
LPDIRECT3DDEVICE9 d3ddev = NULL;    // Direct3D device
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd) {
    const char* CLASS_NAME = "OverlayWindowClass";

    WNDCLASS wc = { };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    RegisterClass(&wc);

    // Find the AssaultCube window and get its client dimensions
    HWND targetWindow = FindWindowA(NULL, "AssaultCube");

    while (!targetWindow) {
        targetWindow = FindWindowA(NULL, "AssaultCube");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    RECT clientRect{ 0, 0, 800, 600 };
    POINT clientToScreenPoint = { 0, 0 };

    if (GetClientRect(targetWindow, &clientRect)) {
        ClientToScreen(targetWindow, &clientToScreenPoint);
    }

    int width = clientRect.right - clientRect.left;
    int height = clientRect.bottom - clientRect.top;

    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW, // Added WS_EX_TOOLWINDOW here
        CLASS_NAME,
        "ImGui Overlay",
        WS_POPUP,
        clientToScreenPoint.x, clientToScreenPoint.y, width, height,  // Use client area dimensions and position
        NULL,
        NULL,
        hInstance,
        NULL
    );

    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    ShowWindow(hwnd, nShowCmd);

    initD3D(hwnd);

    // Initialize the Memory object
    Memory memory("ac_client.exe");

    MSG msg = { };
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            targetWindow = FindWindowA(NULL, "AssaultCube");

            if (!targetWindow) {
                PostQuitMessage(0);
                break;
            }

            // UpdateOverlayPosition(targetWindow, hwnd);
            HandleOverlayVisibility(targetWindow, hwnd);

            // Start the ImGui frame
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // Read the base address, total players, and entity list once per frame
            uintptr_t baseAddress = memory.GetModuleAddress("ac_client.exe");
            int totalPlayers = memory.Read<int>(baseAddress + offsets::totalPlayer);
            uintptr_t entityList = memory.Read<uintptr_t>(baseAddress + offsets::playersEntityList);
            uintptr_t localEntity = memory.Read<uintptr_t>(baseAddress + offsets::localPlayerEntity);
			uintptr_t itemsEntityList = memory.Read<uintptr_t>(baseAddress + offsets::itemsEntityList);
			int gameMode = memory.Read<int>(baseAddress + offsets::gameMode);
			int totalItems = memory.Read<int>(baseAddress + offsets::totalItemPickups);
			int totalEntities = memory.Read<int>(baseAddress + offsets::totalEntity);

            // Optimize by avoiding multiple memory reads
            D3DMATRIX viewMatrix = memory.Read<D3DMATRIX>(baseAddress + offsets::viewMatrix);

            // Player ESP
            for (int i = 0; i < totalPlayers; ++i) {
                uintptr_t entity = memory.Read<uintptr_t>(entityList + (i * 0x4));
                if (entity == 0) continue; // Skip if invalid entity

                if (isTeamGameMode(gameMode))
                {
                    int teamSide = memory.Read<int>(entity + offsets::teamSide);
                    int localTeamSide = memory.Read<int>(localEntity + offsets::teamSide);
                    if (teamSide == localTeamSide) continue; // Skip if the entity is on the same team
                }

                bool isDead = memory.Read<bool>(entity + offsets::isDead);
                if (isDead) continue; // Skip if the entity is dead

                D3DXVECTOR3 headPosition = memory.Read<D3DXVECTOR3>(entity + offsets::head);
                D3DXVECTOR3 footPosition = memory.Read<D3DXVECTOR3>(entity + offsets::foot);
                D3DXVECTOR3 headScreenPosition, footScreenPosition, screenPosition;

                if (WorldToScreen(headPosition, headScreenPosition, viewMatrix, width, height) &&
                    WorldToScreen(footPosition, footScreenPosition, viewMatrix, width, height)) {

                    // Calculate the height and width of the bounding box
                    float boxHeight = footScreenPosition.y - headScreenPosition.y;
                    float boxWidth = boxHeight / 2.0f; // Assuming a 1:2 width to height ratio

                    // Draw the rectangle around the player
                    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
                    draw_list->AddRect(
                        ImVec2(headScreenPosition.x - boxWidth / 2, headScreenPosition.y), // Top-left corner
                        ImVec2(headScreenPosition.x + boxWidth / 2, footScreenPosition.y), // Bottom-right corner
                        ImColor(255, 0, 0)
                    );

                    // Read the player name or any other information to display
					char playerName[16];
					memory.ReadChar<char>(entity + offsets::name, playerName, 16);

                    // Calculate the position to display the player name on the top side of the rectangle
					ImVec2 textSize = ImGui::CalcTextSize(playerName);
                    ImVec2 textPosition = ImVec2(headScreenPosition.x - textSize.x / 2, headScreenPosition.y - 15.0f);

                    // Draw the player name text
                    draw_list->AddText(textPosition, ImColor(255, 255, 255), playerName);

                    // Draw the health bar on the left side of the rectangle
                    float health = static_cast<float>(memory.Read<int>(entity + offsets::health));
                    float maxHealth = 100.0f; // Adjust based on game mechanics
                    float healthBarHeight = boxHeight * (health / maxHealth); // Health bar height proportional to the player height
                    float healthBarWidth = 2.0f; // Width of the health bar

                    ImVec2 healthBarTopLeft = ImVec2((headScreenPosition.x - boxWidth / 2 - healthBarWidth) - 2, headScreenPosition.y);
                    ImVec2 healthBarBottomRight = ImVec2(healthBarTopLeft.x + healthBarWidth, headScreenPosition.y + boxHeight);

                    // Draw the background of the health bar
                    draw_list->AddRectFilled(healthBarTopLeft, healthBarBottomRight, ImColor(0, 0, 0));

                    // Draw the foreground of the health bar based on health percentage
                    ImVec2 healthBarForegroundTopLeft = ImVec2(healthBarTopLeft.x, healthBarTopLeft.y + (boxHeight - healthBarHeight));
                    ImVec2 healthBarForegroundBottomRight = ImVec2(healthBarTopLeft.x + healthBarWidth, healthBarTopLeft.y + boxHeight);
                    draw_list->AddRectFilled(healthBarForegroundTopLeft, healthBarForegroundBottomRight, ImColor(0, 255, 0));

                    // Draw the armor bar on the left side of the health bar
                    float armor = static_cast<float>(memory.Read<int>(entity + offsets::armor));
                    float maxArmor = 100.0f; // Adjust based on game mechanics
                    float armorBarHeight = boxHeight * (armor / maxArmor); // Armor bar height proportional to the player height
                    float armorBarWidth = 2.0f; // Width of the armor bar

                    ImVec2 armorBarTopLeft = ImVec2(healthBarTopLeft.x - armorBarWidth - 2, headScreenPosition.y);
                    ImVec2 armorBarBottomRight = ImVec2(armorBarTopLeft.x + armorBarWidth, headScreenPosition.y + boxHeight);

                    // Draw the background of the armor bar
                    draw_list->AddRectFilled(armorBarTopLeft, armorBarBottomRight, ImColor(0, 0, 0));

                    // Draw the foreground of the armor bar based on armor percentage
                    ImVec2 armorBarForegroundTopLeft = ImVec2(armorBarTopLeft.x, armorBarTopLeft.y + (boxHeight - armorBarHeight));
                    ImVec2 armorBarForegroundBottomRight = ImVec2(armorBarTopLeft.x + armorBarWidth, armorBarTopLeft.y + boxHeight);
                    draw_list->AddRectFilled(armorBarForegroundTopLeft, armorBarForegroundBottomRight, ImColor(255, 255, 255));

                    // Calculate distance from the local player to the target player
                    D3DXVECTOR3 localHeadPosition = memory.Read<D3DXVECTOR3>(localEntity + offsets::head);
                    D3DXVECTOR3 distanceVector = headPosition - localHeadPosition;
                    float distance = D3DXVec3Length(&distanceVector);

                    // Display the distance text at the bottom of the rectangle
                    std::stringstream ss;
                    ss << static_cast<int>(distance) << "m"; // Append " m" to the distance
                    std::string distanceText = ss.str();

                    ImVec2 distanceTextSize = ImGui::CalcTextSize(distanceText.c_str());
                    ImVec2 distanceTextPosition = ImVec2(headScreenPosition.x - distanceTextSize.x / 2, footScreenPosition.y + 1.0f);
                    draw_list->AddText(distanceTextPosition, ImColor(255, 255, 255), distanceText.c_str());
                }
            }

            // Pickups ESP
            for (int i = 0; i < totalEntities; ++i) {
                BYTE itemType = memory.Read<BYTE>(itemsEntityList + offsets::itemType + i * offsets::itemEntitySize);
                BYTE itemAttr2 = memory.Read<BYTE>(itemsEntityList + offsets::itemAttr2 + i * offsets::itemEntitySize);

                if (gameMode == gameModes::CTF) {
                    if (!isItemPickup(itemType) && !isItemRedFlag(itemType, itemAttr2) && !isItemBlueFlag(itemType, itemAttr2)) continue;
                }
                else {
                    if (!isItemPickup(itemType)) continue;
                }

				D3DXVECTOR3 itemScreenPosition;
                Vector3ItemEntity itemPosition = memory.Read<Vector3ItemEntity>(itemsEntityList + offsets::itemPosition + (i * offsets::itemEntitySize));

                if (WorldToScreen(itemPosition.ToD3DXVECTOR3(), itemScreenPosition, viewMatrix, width, height)) {
                    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

                    // Draw text for items
                    char itemText[16];
                    ImColor color;
                    GetItemInfo(itemAttr2, itemType, itemText, color);

                    // Calculate distance from the local player to the item
                    D3DXVECTOR3 localheadPosition = memory.Read<D3DXVECTOR3>(localEntity + offsets::head);
                    D3DXVECTOR3 distanceVector = itemPosition.ToD3DXVECTOR3() - localheadPosition;
                    float distance = D3DXVec3Length(&distanceVector);

                    // Determine the scaling factor for text
                    float textScaleFactor = 1.0f / (distance * 0.05f); // Adjust 0.05f to change scaling intensity
                    textScaleFactor = std::clamp(textScaleFactor, 0.5f, 1.0f); // Limit the scaling factor for readability

                    // Draw the item text at the item's screen position with scaling
                    ImFont* font = ImGui::GetFont();
                    ImVec2 itemTextSize = font->CalcTextSizeA(font->FontSize * textScaleFactor, FLT_MAX, 0.0f, itemText);

                    draw_list->AddText(font, font->FontSize * textScaleFactor,
                        ImVec2(itemScreenPosition.x - itemTextSize.x / 2, itemScreenPosition.y),
                        color, itemText);

                    // Display the distance text at the bottom of the rectangle with scaling
                    std::stringstream ss;
                    ss << static_cast<int>(distance) << "m";
                    std::string distanceText = ss.str();

                    ImVec2 distanceTextSize = font->CalcTextSizeA(font->FontSize * textScaleFactor, FLT_MAX, 0.0f, distanceText.c_str());
                    ImVec2 distanceTextPosition = ImVec2(itemScreenPosition.x - distanceTextSize.x / 2, itemScreenPosition.y + 10.0f);

                    draw_list->AddText(font, font->FontSize * textScaleFactor,
                        distanceTextPosition, color, distanceText.c_str());

                    // Independent scaling for the FLAG rectangle
                    float baseWidth = 5.0f;
                    float baseHeight = 50.0f;

                    float rectScaleFactor = 100.0f / distance; // Adjust 100.0f to control the rectangle scaling effect
                    float rectWidth = baseWidth * rectScaleFactor;
                    float rectHeight = baseHeight * rectScaleFactor;

                    // Draw the FLAG rectangle with adjusted position and independent scaling
                    if (gameMode == gameModes::CTF) {
                        if (isItemRedFlag(itemType, itemAttr2)) {
                            ImVec2 topLeft = ImVec2(itemScreenPosition.x - rectWidth, itemScreenPosition.y - rectHeight / 2);
                            ImVec2 bottomRight = ImVec2(itemScreenPosition.x + rectWidth, itemScreenPosition.y + rectHeight / 2);
                            draw_list->AddRect(topLeft, bottomRight, color);
                        }
                        else if (isItemBlueFlag(itemType, itemAttr2)) {
                            ImVec2 topLeft = ImVec2(itemScreenPosition.x - rectWidth, itemScreenPosition.y - rectHeight / 2);
                            ImVec2 bottomRight = ImVec2(itemScreenPosition.x + rectWidth, itemScreenPosition.y + rectHeight / 2);
                            draw_list->AddRect(topLeft, bottomRight, color);
                        }
                    }
                }
            }

            // Convert the FPS float to a string using stringstream
            std::stringstream ss;
            ss << static_cast<int>(ImGui::GetIO().Framerate);
            std::string fpsText = ss.str();

            // Display the FPS text at (15, 35) without a background box
            ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
            draw_list->AddText(ImVec2(5, 5), ImColor(255, 255, 255), fpsText.c_str());

            // Rendering
            ImGui::Render();
            d3ddev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
            d3ddev->BeginScene();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            d3ddev->EndScene();
            d3ddev->Present(NULL, NULL, NULL, NULL);
        }
    }

    cleanD3D();
    return 0;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam)) {
        return true;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void initD3D(HWND hwnd) {
    d3d = Direct3DCreate9(D3D_SDK_VERSION);

    D3DPRESENT_PARAMETERS d3dpp;
    ZeroMemory(&d3dpp, sizeof(d3dpp));
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.hDeviceWindow = hwnd;

    d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &d3ddev);

    // Setup ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // Setup Platform/Renderer bindings
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(d3ddev);

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
}

void cleanD3D() {
    // Cleanup ImGui
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    d3ddev->Release();
    d3d->Release();
}

void UpdateOverlayPosition(HWND targetWindow, HWND overlayWindow) {
    RECT clientRect;
    POINT clientToScreenPoint = { 0, 0 };

    if (GetClientRect(targetWindow, &clientRect)) {
        // Convert client coordinates to screen coordinates
        ClientToScreen(targetWindow, &clientToScreenPoint);

        int width = clientRect.right - clientRect.left;
        int height = clientRect.bottom - clientRect.top;
        SetWindowPos(overlayWindow, HWND_TOPMOST, clientToScreenPoint.x, clientToScreenPoint.y, width, height, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOCOPYBITS);
    }
}

void HandleOverlayVisibility(HWND targetWindow, HWND overlayWindow) {
    HWND foregroundWindow = GetForegroundWindow();

    if (foregroundWindow == targetWindow) {
        // If AssaultCube is the active window, show the overlay
        ShowWindow(overlayWindow, SW_SHOWNOACTIVATE);
    }
    else {
        // If AssaultCube is not the active window, minimize the overlay
        ShowWindow(overlayWindow, SW_HIDE);
    }
}

bool WorldToScreen(const D3DXVECTOR3& pos, D3DXVECTOR3& screen, const D3DMATRIX& matrix, int width, int height) {
    D3DXVECTOR4 clipCoords;
    clipCoords.x = pos.x * matrix._11 + pos.y * matrix._21 + pos.z * matrix._31 + matrix._41;
    clipCoords.y = pos.x * matrix._12 + pos.y * matrix._22 + pos.z * matrix._32 + matrix._42;
    clipCoords.z = pos.x * matrix._13 + pos.y * matrix._23 + pos.z * matrix._33 + matrix._43;
    clipCoords.w = pos.x * matrix._14 + pos.y * matrix._24 + pos.z * matrix._34 + matrix._44;

    if (clipCoords.w < 0.001f) return false;

    D3DXVECTOR3 NDC;
    NDC.x = clipCoords.x / clipCoords.w;
    NDC.y = clipCoords.y / clipCoords.w;
    NDC.z = clipCoords.z / clipCoords.w;

    screen.x = (width / 2 * NDC.x) + (NDC.x + width / 2);
    screen.y = -(height / 2 * NDC.y) + (NDC.y + height / 2);
    return true;
}

bool isTeamGameMode(int gameMode)
{
    if (gameMode == gameModes::TEAMDEATHMATCH ||
        gameMode == gameModes::TEAMSURVIVOR ||
        gameMode == gameModes::CTF ||
        gameMode == gameModes::BOTTEAMDEATHMATCH ||
        gameMode == gameModes::TEAMONESHOTONEKILL ||
        gameMode == gameModes::HUNTTHEFLAG ||
        gameMode == gameModes::TEAMKEEPTHEFLAG ||
        gameMode == gameModes::TEAMPF ||
        gameMode == gameModes::TEAMLSS ||
        gameMode == gameModes::BOTTEAMSURVIVOR ||
        gameMode == gameModes::BOTTEAMONESHOTONKILL)
    {
        return true;
    }
    return false;
}

bool isItemPickup(int itemType)
{
	if (itemType == itemTypes::I_CLIPS ||
		itemType == itemTypes::I_AMMO ||
		itemType == itemTypes::I_GRENADE ||
		itemType == itemTypes::I_HEALTH ||
		itemType == itemTypes::I_HELMET ||
		itemType == itemTypes::I_ARMOUR ||
		itemType == itemTypes::I_AKIMBO)
	{
		return true;
	}
	return false;
}

bool isItemRedFlag(int itemType, int itemAttr2)
{
	if (itemType == itemTypes::CTF_FLAG && itemAttr2 == 0)
	{
		return true;
	}
	return false;
}

bool isItemBlueFlag(int itemType, int itemAttr2)
{
	if (itemType == itemTypes::CTF_FLAG && itemAttr2 == 1)
	{
		return true;
	}
	return false;
}

void GetItemInfo(int itemAttr2, int itemType, char(&itemName)[16], ImColor& color)
{
    switch (itemType)
    {
    case itemTypes::I_CLIPS:
        strcpy_s(itemName, "Clips");
		color = ImColor(0, 102, 255);
        break;
    case itemTypes::I_AMMO:
        strcpy_s(itemName, "Ammo");
        color = ImColor(0, 153, 255);
        break;
    case itemTypes::I_GRENADE:
        strcpy_s(itemName, "Grenade");
        color = ImColor(255, 0, 0);
        break;
    case itemTypes::I_HEALTH:
        strcpy_s(itemName, "Health");
        color = ImColor(0, 255, 0);
        break;
    case itemTypes::I_HELMET:
        strcpy_s(itemName, "Helmet");
        color = ImColor(255, 255, 255);
        break;
    case itemTypes::I_ARMOUR:
        strcpy_s(itemName, "Armour");
        color = ImColor(255, 255, 255);
        break;
    case itemTypes::I_AKIMBO:
        strcpy_s(itemName, "Akimbo");
        color = ImColor(255, 102, 0);
        break;
    case itemTypes::CTF_FLAG:
        strcpy_s(itemName, "Flag");
        switch (itemAttr2)
        {
		case 0:
			color = ImColor(255, 0, 0);
			break;
		case 1:
			color = ImColor(0, 0, 255);
			break;
        default:
			color = ImColor(255, 255, 255);
            break;
        }
        break;
    default:
        strcpy_s(itemName, "Unknown");
        color = ImColor(255, 255, 255);
        break;
    }
}