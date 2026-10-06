cmake_minimum_required(VERSION 3.24)
project(RD_Desktop_Software LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# Dear ImGui (UI toolkit) - Win32 + DirectX 11 backends
# ---------------------------------------------------------------------------
FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.9b
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${imgui_SOURCE_DIR}/misc/cpp
)
target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)

# ---------------------------------------------------------------------------
# nlohmann/json (header only)
# ---------------------------------------------------------------------------
FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/include.zip
        URL_HASH SHA256=b8cb0ef2dd7f57f18933997c9934bb1fa962594f701cd5a8d3c2c80541559372
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${nlohmann_json_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(RD_Desktop_Software WIN32
        src/main.cpp
        src/App.cpp
        src/Theme.cpp
        src/Settings.cpp
        src/Http.cpp
        src/RepairDeskApi.cpp
        src/Widgets.cpp
        src/DataTable.cpp
        src/pages/InventoryPage.cpp
        src/pages/StockTakePanel.cpp
        src/pages/PosPage.cpp
        src/pages/TicketsPage.cpp
        src/pages/InvoicesPage.cpp
        src/pages/CustomersPage.cpp
        src/pages/CommunicationPage.cpp
        src/pages/SettingsPage.cpp
        src/pages/SearchPage.cpp
        src/pages/RecordViewer.cpp
)
target_include_directories(RD_Desktop_Software PRIVATE src)
target_compile_definitions(RD_Desktop_Software PRIVATE
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)
target_link_libraries(RD_Desktop_Software PRIVATE
        imgui
        nlohmann_json
        winhttp ws2_32
        user32 gdi32 shell32 ole32 uuid winmm
)
if (MINGW)
    # Static C++ runtime so the .exe runs on machines without MinGW installed.
    target_link_options(RD_Desktop_Software PRIVATE -static-libgcc -static-libstdc++
            -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic)
endif ()
