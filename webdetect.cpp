// =============================================================================
// frameworkdetect.cpp  v1.0
//
// Detect which software framework(s) a project uses — from a local directory
// tree OR a live website over the internet.
//
// BUILD
//   Windows (MSVC — "x64 Native Tools Command Prompt"):
//     cl /EHsc /std:c++17 /O2 frameworkdetect.cpp /link winhttp.lib
//
//   Windows (MinGW-w64 / MSYS2):
//     g++ -O2 -std=c++17 -static -o frameworkdetect.exe frameworkdetect.cpp -lwinhttp
//
//   Linux / macOS:
//     g++ -O2 -std=c++17 -pthread frameworkdetect.cpp -lcurl -o frameworkdetect
//     (needs libcurl dev headers: apt install libcurl4-openssl-dev)
//
// USAGE
//   frameworkdetect [targets...] [options]
//   frameworkdetect --help
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <winhttp.h>
    #pragma comment(lib, "winhttp.lib")
#else
    #include <curl/curl.h>
#endif

namespace fs = std::filesystem;
using Clock  = std::chrono::steady_clock;
static constexpr const char* kVersion = "2.0";

// =============================================================================
// ANSI colours
// =============================================================================
namespace col {
    bool enabled = true;
    std::string c(const char* code) { return enabled ? ("\033[" + std::string(code) + "m") : ""; }
    std::string reset()   { return c("0");  }
    std::string bold()    { return c("1");  }
    std::string dim()     { return c("2");  }
    std::string red()     { return c("31"); }
    std::string green()   { return c("32"); }
    std::string yellow()  { return c("33"); }
    std::string blue()    { return c("34"); }
    std::string magenta() { return c("35"); }
    std::string cyan()    { return c("36"); }
}

// =============================================================================
// Utility
// =============================================================================
static std::string toLower(const std::string& s) {
    std::string o(s.size(), '\0');
    std::transform(s.begin(), s.end(), o.begin(), [](unsigned char c){ return std::tolower(c); });
    return o;
}

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::in | std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool suffixMatch(const std::string& name, const std::string& pat) {
    // pat looks like "*.csproj"
    if (pat.size() > 1 && pat[0] == '*') {
        const std::string suf = pat.substr(1);
        if (name.size() < suf.size()) return false;
        const std::string tail = name.substr(name.size() - suf.size());
        return toLower(tail) == toLower(suf);
    }
    return name == pat;
}

static std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 4);
    for (char ch : s) {
        switch (ch) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8]; std::snprintf(buf,sizeof(buf),"\\u%04x",ch); o += buf;
                } else { o += ch; }
        }
    }
    return o;
}

static bool isUrl(const std::string& s) {
    return s.rfind("http://",0)==0 || s.rfind("https://",0)==0;
}

// =============================================================================
// LOCAL-SCAN RULES
// Each rule = (framework, category, config-filename, keywords[], weight)
// filename may start with '*' for suffix matching (e.g. "*.csproj")
// keywords: empty = file presence is enough; otherwise any one keyword hits
// =============================================================================
struct Rule {
    std::string framework, category, configFile;
    std::vector<std::string> keywords;
    int weight;
};

static const std::vector<Rule>& rules() {
    static const std::vector<Rule> R = {
        // ── JavaScript / TypeScript ──────────────────────────────────────────
        {"React",           "Frontend",     "package.json", {"\"react\""},            5},
        {"Next.js",         "Frontend",     "package.json", {"\"next\""},              6},
        {"Vue.js",          "Frontend",     "package.json", {"\"vue\""},               5},
        {"Nuxt.js",         "Frontend",     "package.json", {"\"nuxt\""},              6},
        {"Angular",         "Frontend",     "package.json", {"\"@angular/core\""},     6},
        {"Svelte",          "Frontend",     "package.json", {"\"svelte\""},            5},
        {"SvelteKit",       "Frontend",     "package.json", {"\"@sveltejs/kit\""},     6},
        {"Vite",            "Frontend",     "package.json", {"\"vite\""},              6},
        {"Remix",           "Frontend",     "package.json", {"\"@remix-run/react\""}, 7},
        {"Astro",           "Frontend",     "package.json", {"\"astro\""},             6},
        {"Qwik",            "Frontend",     "package.json", {"\"@builder.io/qwik\""}, 6},
        {"Gatsby",          "Frontend",     "package.json", {"\"gatsby\""},            6},
        {"Solid.js",        "Frontend",     "package.json", {"\"solid-js\""},          6},
        {"Express.js",      "Backend",      "package.json", {"\"express\""},           5},
        {"NestJS",          "Backend",      "package.json", {"\"@nestjs/core\""},      6},
        {"Fastify",         "Backend",      "package.json", {"\"fastify\""},           6},
        {"Hapi.js",         "Backend",      "package.json", {"\"@hapi/hapi\""},        5},
        {"Koa.js",          "Backend",      "package.json", {"\"koa\""},               5},
        {"Electron",        "Desktop",      "package.json", {"\"electron\""},          6},
        {"React Native",    "Mobile",       "package.json", {"\"react-native\""},      6},
        {"Expo",            "Mobile",       "package.json", {"\"expo\""},              6},
        {"Capacitor",       "Mobile",       "package.json", {"\"@capacitor/core\""},   6},
        {"tRPC",            "Backend",      "package.json", {"\"@trpc/server\""},      5},
        {"Bun",             "Runtime",      "package.json", {"\"bun\""},               5},
        {"Prisma",          "ORM",          "package.json", {"\"prisma\""},            4},
        {"Drizzle ORM",     "ORM",          "package.json", {"\"drizzle-orm\""},       4},
        {"Socket.io",       "Realtime",     "package.json", {"\"socket.io\""},         4},
        // Vite config as standalone signal too
        {"Vite",            "Frontend",     "vite.config.ts",  {},                     5},
        {"Vite",            "Frontend",     "vite.config.js",  {},                     5},
        {"Next.js",         "Frontend",     "next.config.js",  {},                     6},
        {"Next.js",         "Frontend",     "next.config.ts",  {},                     6},
        {"Astro",           "Frontend",     "astro.config.mjs",{},                     6},
        {"NestJS",          "Backend",      "nest-cli.json",   {},                     7},
        {"Angular",         "Frontend",     "angular.json",    {},                     7},

        // ── Python ───────────────────────────────────────────────────────────
        {"Django",          "Backend",      "requirements.txt", {"django"},            5},
        {"Flask",           "Backend",      "requirements.txt", {"flask"},             5},
        {"FastAPI",         "Backend",      "requirements.txt", {"fastapi"},           6},
        {"Starlette",       "Backend",      "requirements.txt", {"starlette"},         5},
        {"Tornado",         "Backend",      "requirements.txt", {"tornado"},           5},
        {"Pyramid",         "Backend",      "requirements.txt", {"pyramid"},           5},
        {"Streamlit",       "Frontend",     "requirements.txt", {"streamlit"},         6},
        {"Gradio",          "Frontend",     "requirements.txt", {"gradio"},            6},
        {"PyTorch",         "ML",           "requirements.txt", {"torch"},             5},
        {"TensorFlow",      "ML",           "requirements.txt", {"tensorflow"},        5},
        {"JAX",             "ML",           "requirements.txt", {"jax"},               5},
        {"Scikit-learn",    "ML",           "requirements.txt", {"scikit-learn"},      4},
        {"LangChain",       "AI",           "requirements.txt", {"langchain"},         5},
        {"Django",          "Backend",      "pyproject.toml",   {"django"},            5},
        {"Flask",           "Backend",      "pyproject.toml",   {"flask"},             5},
        {"FastAPI",         "Backend",      "pyproject.toml",   {"fastapi"},           6},
        {"PyTorch",         "ML",           "pyproject.toml",   {"torch"},             5},
        {"Django",          "Backend",      "manage.py",        {},                    7},

        // ── PHP ──────────────────────────────────────────────────────────────
        {"Laravel",         "Backend",      "composer.json", {"laravel/framework"},    7},
        {"Symfony",         "Backend",      "composer.json", {"symfony/framework-bundle"}, 7},
        {"CodeIgniter",     "Backend",      "composer.json", {"codeigniter4/framework"}, 6},
        {"CakePHP",         "Backend",      "composer.json", {"cakephp/cakephp"},      6},
        {"Yii",             "Backend",      "composer.json", {"yiisoft/yii2"},         6},
        {"Slim",            "Backend",      "composer.json", {"slim/slim"},            5},
        {"WordPress",       "CMS",          "wp-config.php", {},                       8},

        // ── Ruby ─────────────────────────────────────────────────────────────
        {"Ruby on Rails",   "Backend",      "Gemfile",      {"rails"},                 7},
        {"Sinatra",         "Backend",      "Gemfile",      {"sinatra"},               5},
        {"Hanami",          "Backend",      "Gemfile",      {"hanami"},                5},
        {"Grape",           "Backend",      "Gemfile",      {"grape"},                 5},

        // ── Java / Kotlin / JVM ──────────────────────────────────────────────
        {"Spring Boot",     "Backend",      "pom.xml",      {"spring-boot"},           7},
        {"Spring",          "Backend",      "pom.xml",      {"springframework"},       5},
        {"Quarkus",         "Backend",      "pom.xml",      {"quarkus"},               6},
        {"Micronaut",       "Backend",      "pom.xml",      {"micronaut"},             6},
        {"Vert.x",          "Backend",      "pom.xml",      {"vertx"},                 6},
        {"Spring Boot",     "Backend",      "build.gradle", {"spring-boot"},           7},
        {"Ktor",            "Backend",      "build.gradle", {"ktor"},                  6},
        {"Android",         "Mobile",       "build.gradle", {"com.android.application","com.android.library"}, 7},

        // ── .NET / C# ────────────────────────────────────────────────────────
        {"ASP.NET Core",    "Backend",      "*.csproj",     {"Microsoft.AspNetCore"},  7},
        {"Blazor",          "Frontend",     "*.csproj",     {"Blazor"},                7},
        {"MAUI",            "Mobile",       "*.csproj",     {"Microsoft.Maui"},        7},
        {"WPF",             "Desktop",      "*.csproj",     {"UseWPF"},                6},
        {"WinForms",        "Desktop",      "*.csproj",     {"UseWindowsForms"},       6},
        {".NET",            "Backend",      "*.csproj",     {"<TargetFramework>"},     3},

        // ── Rust ─────────────────────────────────────────────────────────────
        {"Actix Web",       "Backend",      "Cargo.toml",   {"actix-web"},             6},
        {"Rocket",          "Backend",      "Cargo.toml",   {"rocket"},                6},
        {"Axum",            "Backend",      "Cargo.toml",   {"axum"},                  6},
        {"Warp",            "Backend",      "Cargo.toml",   {"warp"},                  5},
        {"Tauri",           "Desktop",      "Cargo.toml",   {"tauri"},                 6},
        {"Bevy",            "Game",         "Cargo.toml",   {"bevy"},                  6},
        {"Dioxus",          "Frontend",     "Cargo.toml",   {"dioxus"},                6},

        // ── Go ───────────────────────────────────────────────────────────────
        {"Gin",             "Backend",      "go.mod",       {"gin-gonic/gin"},         6},
        {"Echo",            "Backend",      "go.mod",       {"labstack/echo"},         6},
        {"Fiber",           "Backend",      "go.mod",       {"gofiber/fiber"},         6},
        {"Chi",             "Backend",      "go.mod",       {"go-chi/chi"},            5},
        {"Gorilla Mux",     "Backend",      "go.mod",       {"gorilla/mux"},           5},
        {"Buffalo",         "Backend",      "go.mod",       {"gobuffalo/buffalo"},     5},

        // ── Dart / Flutter ───────────────────────────────────────────────────
        {"Flutter",         "Mobile",       "pubspec.yaml", {"flutter:"},              7},

        // ── iOS / macOS ──────────────────────────────────────────────────────
        {"iOS (CocoaPods)", "Mobile",       "Podfile",      {},                        6},
        {"iOS (Xcode)",     "Mobile",       "*.xcodeproj",  {},                        5},

        // ── C / C++ / Native ─────────────────────────────────────────────────
        {"Qt",              "C++/Native",   "CMakeLists.txt",{"find_package(Qt","Qt5","Qt6"}, 7},
        {"Qt",              "C++/Native",   "*.pro",         {},                       6},
        {"Boost",           "C++/Native",   "CMakeLists.txt",{"find_package(Boost","boost::"}, 5},
        {"SFML",            "C++/Native",   "CMakeLists.txt",{"sfml"},                5},
        {"SDL2",            "C++/Native",   "CMakeLists.txt",{"sdl2","find_package(SDL"}, 5},
        {"OpenCV",          "C++/Native",   "CMakeLists.txt",{"opencv"},              5},
        {"CUDA",            "ML",           "CMakeLists.txt",{"cuda","find_package(CUDA"}, 5},
        {"OpenGL",          "Graphics",     "CMakeLists.txt",{"opengl","find_package(OpenGL"}, 4},
        {"Vulkan",          "Graphics",     "CMakeLists.txt",{"vulkan"},              5},
        {"CMake",           "Build System", "CMakeLists.txt",{},                      3},
        {"Unreal Engine",   "Game",         "*.uproject",    {},                      8},
        {"Unity",           "Game",         "ProjectSettings",{},                     7},
        {"Godot",           "Game",         "project.godot", {},                      7},

        // ── Build / Package tools ─────────────────────────────────────────────
        {"Make",            "Build System", "Makefile",      {},                      2},
        {"Ninja",           "Build System", "build.ninja",   {},                      3},
        {"Bazel",           "Build System", "WORKSPACE",     {},                      4},
        {"Bazel",           "Build System", "BUILD.bazel",   {},                      4},
        {"Conan",           "Build System", "conanfile.txt", {},                      3},
        {"Conan",           "Build System", "conanfile.py",  {},                      3},
        {"vcpkg",           "Build System", "vcpkg.json",    {},                      4},
        {"Docker",          "DevOps",       "Dockerfile",    {},                      3},
        {"Docker Compose",  "DevOps",       "docker-compose.yml", {},                 4},
        {"Kubernetes",      "DevOps",       "*.yaml",        {"kind: Deployment","kind: Service","apiVersion:"}, 5},
        {"Terraform",       "DevOps",       "*.tf",          {"terraform {","provider "}, 5},
        {"Ansible",         "DevOps",       "*.yml",         {"hosts:","tasks:","ansible"}, 4},
    };
    return R;
}

// =============================================================================
// WEB-SCAN RULES
// Source: Header = HTTP response headers (lowercased)
//         Cookie = Set-Cookie values (lowercased)
//         Html   = HTML body (lowercased)
// =============================================================================
struct WebRule {
    std::string framework, category;
    enum Source { Header, Cookie, Html } source;
    std::vector<std::string> needles;
    int weight;
};

static const std::vector<WebRule>& webRules() {
    static const std::vector<WebRule> R = {
        // ── Headers ──────────────────────────────────────────────────────────
        {"Express.js",          "Backend",      WebRule::Header, {"x-powered-by: express"},                    7},
        {"ASP.NET",             "Backend",      WebRule::Header, {"x-powered-by: asp.net","x-aspnet-version"}, 7},
        {"PHP",                 "Backend",      WebRule::Header, {"x-powered-by: php"},                        5},
        {"Next.js",             "Frontend",     WebRule::Header, {"x-powered-by: next.js","x-vercel-id"},      7},
        {"Ruby on Rails",       "Backend",      WebRule::Header, {"x-runtime","x-powered-by: phusion passenger"}, 5},
        {"Drupal",              "CMS",          WebRule::Header, {"x-generator: drupal","x-drupal-cache","x-drupal-dynamic-cache"}, 8},
        {"WordPress",           "CMS",          WebRule::Header, {"link: <https://wordpress"},                  6},
        {"Shopify",             "E-Commerce",   WebRule::Header, {"x-shopid","x-shopify-stage"},                8},
        {"Vercel",              "Hosting",      WebRule::Header, {"server: vercel","x-vercel-deployment-url"}, 5},
        {"Netlify",             "Hosting",      WebRule::Header, {"server: netlify","x-nf-request-id"},        5},
        {"Cloudflare",          "Hosting",      WebRule::Header, {"cf-ray"},                                    2},
        {"Firebase Hosting",    "Hosting",      WebRule::Header, {"x-firebase-appcheck","server: firebase hosting"}, 5},
        {"GitHub Pages",        "Hosting",      WebRule::Header, {"server: github.pages"},                     5},
        {"Nginx",               "Web Server",   WebRule::Header, {"server: nginx"},                            2},
        {"Apache",              "Web Server",   WebRule::Header, {"server: apache"},                           2},
        {"Caddy",               "Web Server",   WebRule::Header, {"server: caddy"},                            4},

        // ── Cookies ──────────────────────────────────────────────────────────
        {"Laravel",             "Backend",      WebRule::Cookie, {"laravel_session","xsrf-token"},              7},
        {"Django",              "Backend",      WebRule::Cookie, {"csrftoken","sessionid"},                     6},
        {"Ruby on Rails",       "Backend",      WebRule::Cookie, {"_session_id","_rails_session"},              6},
        {"Express.js",          "Backend",      WebRule::Cookie, {"connect.sid"},                               6},
        {"ASP.NET",             "Backend",      WebRule::Cookie, {"asp.net_sessionid",".aspxauth"},             7},
        {"PHP",                 "Backend",      WebRule::Cookie, {"phpsessid"},                                 5},
        {"Shopify",             "E-Commerce",   WebRule::Cookie, {"_shopify_s","_shopify_y"},                   7},
        {"Magento",             "E-Commerce",   WebRule::Cookie, {"frontend=","mage-cache"},                    7},
        {"WooCommerce",         "E-Commerce",   WebRule::Cookie, {"woocommerce_"},                              6},

        // ── HTML markers ─────────────────────────────────────────────────────
        {"Next.js",             "Frontend",     WebRule::Html,   {"__next_data__","/_next/static/"},            8},
        {"Nuxt.js",             "Frontend",     WebRule::Html,   {"__nuxt__","/_nuxt/"},                        8},
        {"React",               "Frontend",     WebRule::Html,   {"data-reactroot","data-reactid","react-dom"}, 5},
        {"Vue.js",              "Frontend",     WebRule::Html,   {"data-v-","__vue__","vue.runtime"},           5},
        {"Angular",             "Frontend",     WebRule::Html,   {"ng-version=","ng-app="},                     7},
        {"Svelte / SvelteKit",  "Frontend",     WebRule::Html,   {"/_app/immutable/","svelte-"},                7},
        {"Gatsby",              "Frontend",     WebRule::Html,   {"id=\"___gatsby\"","/page-data/"},            8},
        {"Remix",               "Frontend",     WebRule::Html,   {"__remixcontext"},                            8},
        {"Astro",               "Frontend",     WebRule::Html,   {"astro-island","data-astro-"},                7},
        {"Qwik",                "Frontend",     WebRule::Html,   {"q:container","qwik-"},                       7},
        {"Flutter (web)",       "Mobile/Web",   WebRule::Html,   {"flutter_service_worker","flutter.js"},       8},
        {"WordPress",           "CMS",          WebRule::Html,   {"/wp-content/","/wp-includes/","wp-json"},    8},
        {"WooCommerce",         "E-Commerce",   WebRule::Html,   {"woocommerce"},                               6},
        {"Drupal",              "CMS",          WebRule::Html,   {"drupal.settings","/sites/default/files/"},   8},
        {"Joomla",              "CMS",          WebRule::Html,   {"/components/com_","joomla!"},                8},
        {"Shopify",             "E-Commerce",   WebRule::Html,   {"cdn.shopify.com","shopify.theme"},           8},
        {"Magento",             "E-Commerce",   WebRule::Html,   {"/skin/frontend/","mage.cookies","/static/version"}, 7},
        {"Squarespace",         "Site Builder", WebRule::Html,   {"static1.squarespace.com","squarespace-cdn"}, 8},
        {"Wix",                 "Site Builder", WebRule::Html,   {"static.wixstatic.com","wix-warmup-data"},    8},
        {"Webflow",             "Site Builder", WebRule::Html,   {"data-wf-site","webflow.com"},                8},
        {"Hugo",                "Static Site",  WebRule::Html,   {"name=\"generator\" content=\"hugo"},         7},
        {"Jekyll",              "Static Site",  WebRule::Html,   {"name=\"generator\" content=\"jekyll"},       7},
        {"Eleventy",            "Static Site",  WebRule::Html,   {"name=\"generator\" content=\"eleventy"},     7},
        {"Ghost",               "CMS",          WebRule::Html,   {"ghost-url","content=\"ghost"},               7},
        {"Ruby on Rails",       "Backend",      WebRule::Html,   {"csrf-param\" content=\"authenticity_token","data-turbo"}, 6},
        {"ASP.NET",             "Backend",      WebRule::Html,   {"__viewstate","__eventvalidation"},           7},
        {"Django",              "Backend",      WebRule::Html,   {"csrfmiddlewaretoken"},                       6},
        {"jQuery",              "Library",      WebRule::Html,   {"jquery.min.js","jquery.js"},                 3},
        {"Bootstrap",           "CSS Fw",       WebRule::Html,   {"bootstrap.min.css","bootstrap.bundle"},      3},
        {"Tailwind CSS",        "CSS Fw",       WebRule::Html,   {"tailwindcss"},                               3},
        {"Framer Motion",       "Library",      WebRule::Html,   {"framer-motion"},                             3},
    };
    return R;
}

// =============================================================================
// HTTP FETCH  (WinHTTP on Windows, libcurl everywhere else)
// =============================================================================
struct FetchOptions {
    long timeoutSec = 10;
    bool insecure   = false;
    std::string userAgent = "Mozilla/5.0 (compatible; frameworkdetect/" +
                             std::string(kVersion) + ")";
};

struct FetchResult {
    bool ok = false;
    long httpCode = 0;
    std::string headers, body, finalUrl, error;
};

// ── Windows ──────────────────────────────────────────────────────────────────
#ifdef _WIN32
static std::wstring toWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
static std::string toUtf8(const wchar_t* w, int n) {
    if (n <= 0) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), len, nullptr, nullptr);
    return s;
}

static FetchResult fetchUrl(const std::string& url, const FetchOptions& o) {
    FetchResult res;
    std::wstring wurl = toWide(url);
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t host[512]={}, path[4096]={}, extra[4096]={};
    uc.lpszHostName=host; uc.dwHostNameLength=512;
    uc.lpszUrlPath=path;  uc.dwUrlPathLength=4096;
    uc.lpszExtraInfo=extra; uc.dwExtraInfoLength=4096;
    if (!WinHttpCrackUrl(wurl.c_str(),(DWORD)wurl.size(),0,&uc)){
        res.error="could not parse URL"; return res;
    }
    bool sec = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    std::wstring obj = std::wstring(path)+std::wstring(extra);
    if (obj.empty()) obj = L"/";

    HINTERNET hS = WinHttpOpen(toWide(o.userAgent).c_str(),
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if (!hS){ res.error="WinHttpOpen failed"; return res; }

    DWORD tms = (DWORD)std::max<long>(1,o.timeoutSec)*1000;
    DWORD cms = std::min<DWORD>(tms,6000);
    WinHttpSetTimeouts(hS,cms,cms,tms,tms);

    HINTERNET hC = WinHttpConnect(hS,host,uc.nPort,0);
    if (!hC){ WinHttpCloseHandle(hS); res.error="WinHttpConnect failed"; return res; }

    DWORD flags = sec ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hR = WinHttpOpenRequest(hC,L"GET",obj.c_str(),nullptr,
        WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,flags);
    if (!hR){ WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
              res.error="WinHttpOpenRequest failed"; return res; }

    if (o.insecure && sec){
        DWORD sf = SECURITY_FLAG_IGNORE_UNKNOWN_CA|SECURITY_FLAG_IGNORE_CERT_DATE_INVALID|
                    SECURITY_FLAG_IGNORE_CERT_CN_INVALID|SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hR,WINHTTP_OPTION_SECURITY_FLAGS,&sf,sizeof(sf));
    }
#ifdef WINHTTP_OPTION_DECOMPRESSION
    DWORD d=WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(hR,WINHTTP_OPTION_DECOMPRESSION,&d,sizeof(d));
#endif
    DWORD rp=WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hR,WINHTTP_OPTION_REDIRECT_POLICY,&rp,sizeof(rp));

    if (!WinHttpSendRequest(hR,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||
        !WinHttpReceiveResponse(hR,nullptr)){
        DWORD e=GetLastError();
        res.error="request failed (WinHTTP error "+std::to_string(e)+")";
        WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
        return res;
    }
    DWORD sc=0,ss=sizeof(sc);
    WinHttpQueryHeaders(hR,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                         WINHTTP_HEADER_NAME_BY_INDEX,&sc,&ss,WINHTTP_NO_HEADER_INDEX);
    DWORD hs=0;
    WinHttpQueryHeaders(hR,WINHTTP_QUERY_RAW_HEADERS_CRLF,WINHTTP_HEADER_NAME_BY_INDEX,
                         WINHTTP_NO_OUTPUT_BUFFER,&hs,WINHTTP_NO_HEADER_INDEX);
    if (hs){ std::wstring hw(hs/sizeof(wchar_t),0);
        if (WinHttpQueryHeaders(hR,WINHTTP_QUERY_RAW_HEADERS_CRLF,WINHTTP_HEADER_NAME_BY_INDEX,
                                 hw.data(),&hs,WINHTTP_NO_HEADER_INDEX))
            res.headers=toUtf8(hw.c_str(),(int)hw.size()); }
    DWORD us=0;
    WinHttpQueryOption(hR,WINHTTP_OPTION_URL,nullptr,&us);
    if (us){ std::wstring uw(us/sizeof(wchar_t),0);
        if (WinHttpQueryOption(hR,WINHTTP_OPTION_URL,uw.data(),&us))
            res.finalUrl=toUtf8(uw.c_str(),(int)uw.size()); }
    if (res.finalUrl.empty()) res.finalUrl=url;
    constexpr DWORD kMaxBody=2*1024*1024;
    DWORD av=0;
    while (WinHttpQueryDataAvailable(hR,&av)&&av>0&&res.body.size()<kMaxBody){
        std::vector<char> buf(av); DWORD rd=0;
        if (!WinHttpReadData(hR,buf.data(),av,&rd)||rd==0) break;
        res.body.append(buf.data(),rd);
    }
    res.ok=true; res.httpCode=(long)sc;
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
    return res;
}
static void netInit()    {}
static void netCleanup() {}

// ── Linux / macOS ────────────────────────────────────────────────────────────
#else
static size_t curlCB(char* p,size_t s,size_t n,void* u){
    static_cast<std::string*>(u)->append(p,s*n); return s*n;
}
static FetchResult fetchUrl(const std::string& url, const FetchOptions& o) {
    FetchResult res;
    CURL* curl = curl_easy_init();
    if (!curl){ res.error="curl_easy_init failed"; return res; }
    std::string body,hdr; char errbuf[CURL_ERROR_SIZE]={};
    curl_easy_setopt(curl,CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,  curlCB);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,      &body);
    curl_easy_setopt(curl,CURLOPT_HEADERFUNCTION, curlCB);
    curl_easy_setopt(curl,CURLOPT_HEADERDATA,     &hdr);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl,CURLOPT_MAXREDIRS,      5L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,        o.timeoutSec);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT, std::min<long>(o.timeoutSec,6L));
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER, o.insecure?0L:1L);
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST, o.insecure?0L:2L);
    curl_easy_setopt(curl,CURLOPT_ACCEPT_ENCODING,"");
    curl_easy_setopt(curl,CURLOPT_USERAGENT,      o.userAgent.c_str());
    curl_easy_setopt(curl,CURLOPT_ERRORBUFFER,    errbuf);
    curl_easy_setopt(curl,CURLOPT_BUFFERSIZE,     65536L);
    CURLcode rc = curl_easy_perform(curl);
    if (rc!=CURLE_OK){ res.error=strlen(errbuf)?errbuf:curl_easy_strerror(rc);
                        curl_easy_cleanup(curl); return res; }
    long code=0; curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&code);
    char* eff=nullptr; curl_easy_getinfo(curl,CURLINFO_EFFECTIVE_URL,&eff);
    res.ok=true; res.httpCode=code;
    res.finalUrl=eff?eff:url; res.headers=hdr; res.body=body;
    curl_easy_cleanup(curl);
    return res;
}
static void netInit()    { curl_global_init(CURL_GLOBAL_DEFAULT); }
static void netCleanup() { curl_global_cleanup(); }
#endif

// =============================================================================
// RESULT MODEL — shared between directory and website scanning
// =============================================================================
struct Match {
    std::string framework, category;
    int score=0, confidence=0;  // confidence = 0..100 relative to top match
    std::vector<std::string> evidence;
};

struct ScanResult {
    std::string type;    // "directory" | "website"
    std::string target;
    bool ok = true;
    std::string error;
    double elapsedMs = 0;
    long httpCode = 0;       // website only
    std::string finalUrl;    // website only
    std::vector<Match> matches; // ranked, best first
};

struct Hit { std::string framework, category, evidence; int weight; };

static std::vector<Match> buildMatches(std::vector<Hit>& hits) {
    struct Agg { int score=0; std::string cat; std::vector<std::string> ev; };
    std::unordered_map<std::string,Agg> agg;
    for (auto& h : hits) {
        auto& a=agg[h.framework]; a.score+=h.weight; a.cat=h.category; a.ev.push_back(h.evidence);
    }
    std::vector<std::pair<std::string,Agg>> pairs(agg.begin(),agg.end());
    std::sort(pairs.begin(),pairs.end(),[](auto& a,auto& b){ return a.second.score>b.second.score; });
    int top = pairs.empty()?0:pairs.front().second.score;
    std::vector<Match> out;
    for (auto& [name,a]:pairs)
        out.push_back({name,a.cat,a.score,
                       top>0?(int)std::min(100.0,100.0*a.score/top):0,
                       std::move(a.ev)});
    return out;
}

// =============================================================================
// DIRECTORY SCAN
// =============================================================================
// Dir names we never descend into
static const std::unordered_set<std::string> kSkipDirs = {
    ".git",".hg",".svn","node_modules","vendor","venv",".venv",
    "__pycache__","build","dist","out","target","bin","obj",
    ".gradle",".idea",".vscode","Pods","DerivedData",
    "cmake-build-debug","cmake-build-release",
    ".next",".nuxt","coverage","tmp","temp",".cache"
};

struct DirOptions {
    int maxDepth = -1;
    std::unordered_set<std::string> extraExclude;
    unsigned threadCap = 0; // 0 = no cap
};

static ScanResult scanDir(const fs::path& root, const DirOptions& dopt) {
    ScanResult r; r.type="directory"; r.target=root.string();
    if (!fs::exists(root)){ r.ok=false; r.error="path does not exist"; return r; }
    auto t0=Clock::now();

    std::unordered_map<std::string,std::vector<fs::path>> byName;
    std::vector<fs::path> suffixed; // .csproj .pro .xcodeproj .uproject .tf .yaml .yml
    static const std::unordered_set<std::string> kSufExts={
        ".csproj",".pro",".xcodeproj",".uproject",".tf",".yaml",".yml"
    };

    std::error_code ec;
    for (auto it=fs::recursive_directory_iterator(root,
             fs::directory_options::skip_permission_denied,ec);
         it!=fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec){ ec.clear(); continue; }
        const auto& e=*it;
        if (e.is_directory(ec)){
            const std::string dn=e.path().filename().string();
            bool prune=kSkipDirs.count(dn)||dopt.extraExclude.count(dn);
            bool deep=(dopt.maxDepth>=0 && it.depth()>=dopt.maxDepth);
            if (prune||deep) it.disable_recursion_pending();
            continue;
        }
        if (!e.is_regular_file(ec)) continue;
        const std::string fn=e.path().filename().string();
        byName[fn].push_back(e.path());
        if (kSufExts.count(toLower(e.path().extension().string())))
            suffixed.push_back(e.path());
    }

    // Unity: ProjectSettings/ is a directory
    bool hasUnity = fs::exists(root/"ProjectSettings",ec) &&
                    fs::is_directory(root/"ProjectSettings",ec);
    // Godot: project.godot file
    bool hasGodot = byName.count("project.godot")>0;

    // Collect files we need to actually read
    std::unordered_set<std::string> toRead;
    for (const auto& rule:rules()){
        if (rule.keywords.empty()) continue;
        if (rule.configFile[0]=='*') continue;
        if (byName.count(rule.configFile)) toRead.insert(rule.configFile);
    }

    std::unordered_map<std::string,std::string> cache; // fname -> lowercased concat
    std::mutex mu;
    {
        std::vector<std::string> names(toRead.begin(),toRead.end());
        size_t bs=dopt.threadCap>0?(size_t)dopt.threadCap:std::max<size_t>(1,names.size());
        for (size_t s=0;s<names.size();s+=bs){
            std::vector<std::future<void>> futs;
            for (size_t i=s;i<std::min(s+bs,names.size());++i){
                const std::string fn=names[i];
                futs.push_back(std::async(std::launch::async,[&,fn](){
                    std::string combined;
                    for (const auto& p:byName.at(fn)){ combined+=toLower(readFile(p)); combined+='\n'; }
                    std::lock_guard<std::mutex> lk(mu); cache[fn]=std::move(combined);
                }));
            }
            for (auto& f:futs) f.get();
        }
    }

    std::vector<Hit> hits;

    for (const auto& rule:rules()){
        const std::string& cf=rule.configFile;
        if (cf=="ProjectSettings"){ if(hasUnity) hits.push_back({rule.framework,rule.category,"ProjectSettings/ dir found",rule.weight}); continue; }
        if (cf=="project.godot"){   if(hasGodot) hits.push_back({rule.framework,rule.category,"project.godot found",rule.weight}); continue; }
        if (cf[0]=='*'){
            for (const auto& p:suffixed){
                if (!suffixMatch(p.filename().string(),cf)) continue;
                if (rule.keywords.empty()){ hits.push_back({rule.framework,rule.category,p.filename().string()+" found",rule.weight}); break; }
                std::string content=toLower(readFile(p));
                for (const auto& kw:rule.keywords)
                    if (content.find(toLower(kw))!=std::string::npos){
                        hits.push_back({rule.framework,rule.category,p.filename().string()+" has \""+kw+"\"",rule.weight}); goto nextSufRule;
                    }
                nextSufRule:;
            }
            continue;
        }
        if (!byName.count(cf)) continue;
        if (rule.keywords.empty()){ hits.push_back({rule.framework,rule.category,cf+" found",rule.weight}); continue; }
        const std::string& content=cache[cf];
        for (const auto& kw:rule.keywords)
            if (content.find(toLower(kw))!=std::string::npos){
                hits.push_back({rule.framework,rule.category,cf+" has \""+kw+"\"",rule.weight}); break;
            }
    }

    r.elapsedMs=std::chrono::duration<double,std::milli>(Clock::now()-t0).count();
    r.matches=buildMatches(hits);
    return r;
}

// =============================================================================
// WEBSITE SCAN
// =============================================================================
static ScanResult scanWeb(const std::string& url, const FetchOptions& fopt) {
    ScanResult r; r.type="website"; r.target=url;
    netInit();
    auto t0=Clock::now();
    FetchResult fr=fetchUrl(url,fopt);
    r.elapsedMs=std::chrono::duration<double,std::milli>(Clock::now()-t0).count();
    netCleanup();
    if (!fr.ok){ r.ok=false; r.error=fr.error; return r; }
    r.httpCode=fr.httpCode; r.finalUrl=fr.finalUrl;

    const std::string hdr=toLower(fr.headers);
    const std::string body=toLower(fr.body);
    std::string cookies;
    { std::istringstream ss(hdr); std::string line;
      while(std::getline(ss,line)) if(line.rfind("set-cookie:",0)==0){ cookies+=line; cookies+='\n'; } }

    std::vector<Hit> hits;
    for (const auto& wr:webRules()){
        const std::string* hay=nullptr;
        switch(wr.source){
            case WebRule::Header: hay=&hdr;     break;
            case WebRule::Cookie: hay=&cookies; break;
            case WebRule::Html:   hay=&body;    break;
        }
        for (const auto& n:wr.needles)
            if (hay->find(toLower(n))!=std::string::npos){
                hits.push_back({wr.framework,wr.category,"matched \""+n+"\"",wr.weight}); break;
            }
    }
    r.matches=buildMatches(hits);
    return r;
}

// =============================================================================
// PRINT: human-readable
// =============================================================================
struct PrintOpts {
    bool all=false, quiet=false, jsonMode=false;
    int minConf=0;
    std::string catFilter; // "" = all
};

static std::string categoryColor(const std::string& cat) {
    if (cat=="Frontend"||cat=="CSS Fw")           return col::cyan();
    if (cat=="Backend")                           return col::green();
    if (cat=="ML"||cat=="AI")                     return col::magenta();
    if (cat=="Mobile"||cat=="Mobile/Web")         return col::yellow();
    if (cat=="Game")                              return col::red();
    if (cat=="DevOps"||cat=="Build System")       return col::dim();
    return "";
}

static void printHuman(const ScanResult& r, const PrintOpts& p) {
    // ── quiet mode ───────────────────────────────────────────────────────────
    if (p.quiet) {
        if (!r.ok)            { std::cout<<r.target<<": ERROR ("<<r.error<<")\n"; return; }
        if (r.matches.empty()){ std::cout<<r.target<<": no framework detected\n"; return; }
        const auto& m=r.matches.front();
        std::cout<<r.target<<": "<<col::bold()<<m.framework<<col::reset()
                  <<" ["<<m.category<<"] "<<m.confidence<<"%\n";
        return;
    }

    // ── header ───────────────────────────────────────────────────────────────
    std::cout<<"\n"<<col::bold()<<col::cyan()<<"┌─ frameworkdetect "<<kVersion<<" "<<col::reset();
    std::cout<<col::dim()<<"─────────────────────────────────────"<<col::reset()<<"\n";
    std::cout<<col::bold()<<"│ Target: "<<col::reset()<<r.target<<"\n";

    if (!r.ok) {
        std::cout<<col::bold()<<"│ "<<col::red()<<"ERROR: "<<col::reset()<<r.error<<"\n";
        std::cout<<col::dim()<<"└──────────────────────────────────────────────────"<<col::reset()<<"\n\n";
        return;
    }

    if (r.type=="website")
        std::cout<<col::bold()<<"│ "<<col::reset()<<col::dim()<<"HTTP "<<r.httpCode<<" · "
                  <<r.elapsedMs<<"ms · "<<r.finalUrl<<col::reset()<<"\n";
    else
        std::cout<<col::bold()<<"│ "<<col::reset()<<col::dim()<<"scanned in "
                  <<r.elapsedMs<<"ms"<<col::reset()<<"\n";

    std::cout<<col::dim()<<"├──────────────────────────────────────────────────"<<col::reset()<<"\n";

    if (r.matches.empty()) {
        std::cout<<col::bold()<<"│ "<<col::yellow()<<"No framework detected"<<col::reset()<<"\n";
        if (r.type=="website")
            std::cout<<col::dim()<<"│ Tip: site may use heavy client-side rendering or a custom stack\n"<<col::reset();
        std::cout<<col::dim()<<"└──────────────────────────────────────────────────"<<col::reset()<<"\n\n";
        return;
    }

    int topScore=r.matches.front().score;
    bool any=false;
    for (const auto& m:r.matches) {
        if (!p.catFilter.empty() && toLower(m.category)!=toLower(p.catFilter)) continue;
        bool isTop=(m.score==topScore);
        if (!p.all && !isTop) continue;
        if (!isTop && m.confidence<p.minConf) continue;

        std::cout<<col::bold()<<"│ "<<col::reset();
        std::cout<<(isTop?col::bold():"")<<categoryColor(m.category)
                  <<m.framework<<col::reset()
                  <<"  "<<col::dim()<<"["<<m.category<<"]"<<col::reset()
                  <<"  score="<<col::bold()<<m.score<<col::reset()
                  <<"  conf="<<col::bold()<<m.confidence<<"%"<<col::reset();
        if (isTop) std::cout<<"  "<<col::green()<<"✓"<<col::reset();
        std::cout<<"\n";

        if (p.all) for (const auto& ev:m.evidence)
            std::cout<<col::dim()<<"│   · "<<ev<<col::reset()<<"\n";
        any=true;
    }
    if (!any && !p.catFilter.empty())
        std::cout<<col::dim()<<"│ (no matches in category \""<<p.catFilter<<"\")"<<col::reset()<<"\n";

    if (!p.all) {
        size_t hidden=0;
        for (const auto& m:r.matches) if (m.score!=topScore) ++hidden;
        if (hidden)
            std::cout<<col::dim()<<"│ "<<hidden<<" more lower-confidence match(es) — use --all to show"<<col::reset()<<"\n";
    }
    std::cout<<col::dim()<<"└──────────────────────────────────────────────────"<<col::reset()<<"\n";
}

// =============================================================================
// PRINT: JSON
// =============================================================================
static std::string toJson(const ScanResult& r, const PrintOpts& p) {
    std::ostringstream j;
    j<<"{\"type\":\""<<jsonEscape(r.type)<<"\","
      <<"\"target\":\""<<jsonEscape(r.target)<<"\","
      <<"\"ok\":"<<(r.ok?"true":"false");
    if (!r.ok){ j<<",\"error\":\""<<jsonEscape(r.error)<<"\""; j<<"}"; return j.str(); }
    j<<",\"elapsed_ms\":"<<r.elapsedMs;
    if (r.type=="website"){ j<<",\"http_code\":"<<r.httpCode<<",\"final_url\":\""<<jsonEscape(r.finalUrl)<<"\""; }
    j<<",\"matches\":[";
    int topScore=r.matches.empty()?0:r.matches.front().score;
    bool first=true;
    for (const auto& m:r.matches){
        if (!p.catFilter.empty() && toLower(m.category)!=toLower(p.catFilter)) continue;
        if (!p.all && m.score!=topScore) continue;
        if (m.score!=topScore && m.confidence<p.minConf) continue;
        if (!first) j<<","; first=false;
        j<<"{\"framework\":\""<<jsonEscape(m.framework)<<"\","
          <<"\"category\":\""<<jsonEscape(m.category)<<"\","
          <<"\"score\":"<<m.score<<","
          <<"\"confidence\":"<<m.confidence;
        if (p.all){
            j<<",\"evidence\":[";
            for (size_t i=0;i<m.evidence.size();++i){ if(i)j<<","; j<<"\""<<jsonEscape(m.evidence[i])<<"\""; }
            j<<"]";
        }
        j<<"}";
    }
    j<<"]}"; return j.str();
}

// =============================================================================
// HELP
// =============================================================================
static void printHelp(const char* prog) {
    std::cout
        <<col::bold()<<col::cyan()<<"frameworkdetect "<<kVersion<<col::reset()
        <<"  —  detect the software framework behind any project or website\n\n"
        <<col::bold()<<"USAGE\n"<<col::reset()
        <<"  "<<prog<<" [target ...] [OPTIONS]\n"
        <<"  "<<prog<<"                      "<<col::dim()<<"# scans current directory\n"<<col::reset()
        <<"\n"
        <<col::bold()<<"TARGETS  (mix freely, repeat as needed)\n"<<col::reset()
        <<"  /path/to/project       Scan a local source tree\n"
        <<"  https://example.com    Probe a live website\n"
        <<"\n"
        <<col::bold()<<"OUTPUT\n"<<col::reset()
        <<"  -h, --help              Show this message and exit\n"
        <<"  -V, --version           Print version and exit\n"
        <<"  --all                   Show all matches, not just the top tier\n"
        <<"  --min-confidence N      With --all, hide matches below N% confidence\n"
        <<"  -c, --category CAT      Filter output to a specific category\n"
        <<"                          e.g. Frontend  Backend  ML  Mobile  Game\n"
        <<"                               DevOps  C++/Native  Build System\n"
        <<"  -q, --quiet             One line per target (good for scripts)\n"
        <<"  --json                  Machine-readable JSON array output\n"
        <<"  --no-color              Disable ANSI colours\n"
        <<"\n"
        <<col::bold()<<"LOCAL SCAN\n"<<col::reset()
        <<"  --depth N               Limit directory recursion to N levels\n"
        <<"  --exclude NAME          Also skip directories named NAME (repeatable)\n"
        <<"  --threads N             Cap parallel file-read threads (default: auto)\n"
        <<"\n"
        <<col::bold()<<"WEBSITE SCAN\n"<<col::reset()
        <<"  --timeout SEC           Network timeout in seconds (default 10)\n"
        <<"  --insecure              Skip TLS certificate verification\n"
        <<"  --user-agent UA         Override the HTTP User-Agent string\n"
        <<"\n"
        <<col::bold()<<"EXAMPLES\n"<<col::reset()
        <<"  "<<prog<<"\n"
        <<"  "<<prog<<" ~/myproject --all --json\n"
        <<"  "<<prog<<" ~/myproject --depth 4 --exclude legacy\n"
        <<"  "<<prog<<" https://shopify.com --all\n"
        <<"  "<<prog<<" /repo1 /repo2 /repo3 --quiet\n"
        <<"  "<<prog<<" https://a.com /repo --json > report.json\n"
        <<"  "<<prog<<" . --all -c Backend\n"
        <<"  "<<prog<<" https://example.com --timeout 20 --insecure\n"
        <<"\n"
        <<col::bold()<<"FRAMEWORK CATEGORIES\n"<<col::reset()
        <<"  Frontend  Backend  Mobile  Game  Desktop  ML  AI  Static Site\n"
        <<"  CMS  E-Commerce  Site Builder  DevOps  Build System  Graphics\n"
        <<"  C++/Native  Library  ORM  Realtime  Runtime  Web Server  Hosting\n"
        <<"\n"
        <<col::dim()
        <<"  Local scan checks: package.json, CMakeLists.txt, pom.xml, Cargo.toml,\n"
        <<"  requirements.txt, go.mod, *.csproj, pubspec.yaml, Gemfile, Dockerfile,\n"
        <<"  docker-compose.yml, *.tf, *.yaml and more (~100 rules, 70+ frameworks).\n"
        <<"  Website scan checks HTTP headers, cookies, and HTML body markers.\n"
        <<col::reset();
}

// =============================================================================
// MAIN
// =============================================================================
int main(int argc, char** argv) {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode  = 0;
    if (hOut!=INVALID_HANDLE_VALUE && GetConsoleMode(hOut,&mode))
        SetConsoleMode(hOut, mode|ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    else col::enabled = false;
#endif

    std::vector<std::string> targets;
    PrintOpts popt;
    DirOptions dopt;
    FetchOptions fopt;

    auto needsArg=[&](int i)->bool{
        if (i+1>=argc){ std::cerr<<"Missing value for "<<argv[i]<<"\n"; return false; }
        return true;
    };

    for (int i=1;i<argc;++i){
        std::string a=argv[i];
        if      (a=="-h"||a=="--help"||a=="/?") { col::enabled=true; printHelp(argv[0]); return 0; }
        else if (a=="-V"||a=="--version")        { std::cout<<"frameworkdetect "<<kVersion<<"\n"; return 0; }
        else if (a=="--all")                     popt.all=true;
        else if (a=="--no-color")                col::enabled=false;
        else if (a=="-q"||a=="--quiet")          popt.quiet=true;
        else if (a=="--json")                    popt.jsonMode=true;
        else if (a=="--insecure")                fopt.insecure=true;
        else if (a=="--min-confidence"){ if(!needsArg(i))return 1; popt.minConf=std::atoi(argv[++i]); }
        else if (a=="-c"||a=="--category"){ if(!needsArg(i))return 1; popt.catFilter=argv[++i]; }
        else if (a=="--depth"){   if(!needsArg(i))return 1; dopt.maxDepth=std::atoi(argv[++i]); }
        else if (a=="--exclude"){ if(!needsArg(i))return 1; dopt.extraExclude.insert(argv[++i]); }
        else if (a=="--threads"){ if(!needsArg(i))return 1; dopt.threadCap=(unsigned)std::max(0,std::atoi(argv[++i])); }
        else if (a=="--timeout"){ if(!needsArg(i))return 1; fopt.timeoutSec=std::max(1,std::atoi(argv[++i])); }
        else if (a=="--user-agent"){ if(!needsArg(i))return 1; fopt.userAgent=argv[++i]; }
        else if (!a.empty()&&a[0]=='-'){ std::cerr<<"Unknown option: "<<a<<" (try --help)\n"; return 1; }
        else targets.push_back(a);
    }
    if (targets.empty()) targets.push_back(".");

    std::vector<ScanResult> results;
    results.reserve(targets.size());
    for (const auto& t:targets)
        results.push_back(isUrl(t)?scanWeb(t,fopt):scanDir(fs::path(t),dopt));

    if (popt.jsonMode){
        std::cout<<"[\n";
        for (size_t i=0;i<results.size();++i){
            std::cout<<"  "<<toJson(results[i],popt);
            if (i+1<results.size()) std::cout<<",";
            std::cout<<"\n";
        }
        std::cout<<"]\n";
    } else {
        for (auto& r:results) printHuman(r,popt);
    }

    return std::any_of(results.begin(),results.end(),[](const ScanResult& r){return !r.ok;})?1:0;
}