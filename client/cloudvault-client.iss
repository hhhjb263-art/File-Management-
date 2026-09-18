; ===========================================================================
;  CloudVault 测试客户端 - Inno Setup 安装包脚本
;  编译环境：Inno Setup 6.x (Unicode版)
;  使用方法：用 Inno Setup Compiler (ISCC) 打开本文件编译，
;        或者命令行执行：
;        ISCC.exe "D:/Qtproject/File/client/cloudvault-client.iss"
;
;  打包源目录 build_sec2 已经通过 windeployqt 部署全部Qt运行库，
;  脚本直接拷贝文件 + 插件子目录，已过滤 CMake 构建产物。
;
;  路径全部使用正斜杠 /，避免 Inno 预处理器把 \b\f\n 等反斜杠字符当成转义符。
;  本版本不创建任何桌面图标、开始菜单图标、快速启动栏图标。
; ===========================================================================

#define MyAppName        "CloudVault 测试客户端"
#define MyAppNameEn      "cloudvault-client"
#define MyAppVersion     "1.0.0"
#define MyAppPublisher   "cloudvault"
#define MyAppURL         "https://cloudvault.local"

; 打包源根目录（windeployqt 部署输出目录）
#define MySourceRoot     "D:/Qtproject/File/client/build_sec2"

; 安装包输出目录
#define MyOutputDir      "D:/Qtproject/File/client/build_exe"

[Setup]
; --- 基础信息 ---
AppId={{7F3A9C2E-1B4D-4E8A-9C6F-2D5E8A1B3C4D}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}/CloudVault
DefaultGroupName={#MyAppName}

; 64位 MinGW 构建，强制64位安装模式
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64

; --- 安装包输出 ---
OutputDir={#MyOutputDir}
OutputBaseFilename=cloudvault-client-setup-{#MyAppVersion}
UninstallDisplayIcon={app}/{#MyAppNameEn}.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; 安装到 Program Files 需要管理员权限
PrivilegesRequired=admin

; 不创建开始菜单文件夹中的程序图标，也不允许无图标安装
AllowNoIcons=no
DisableProgramGroupPage=no

; 升级/重装时自动关闭正在运行的程序，避免文件占用
CloseApplications=yes
RestartApplications=no
UsedUserAreasWarning=no

; --- 版本信息资源 ---
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} 安装程序
VersionInfoTextVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoProductTextVersion={#MyAppVersion}
VersionInfoCopyright=Copyright (C) {#MyAppPublisher}

[Languages]
;Name: "chinesesimplified"; MessagesFile: "compiler:Languages/ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; ---------------------------------------------------------------------------
;  主程序
; ---------------------------------------------------------------------------
Source: "{#MySourceRoot}/{#MyAppNameEn}.exe"; DestDir: "{app}"; Flags: ignoreversion

; ---------------------------------------------------------------------------
;  Qt 6 核心模块 DLL
; ---------------------------------------------------------------------------
Source: "{#MySourceRoot}/Qt6Core.dll";    DestDir: "{app}"; Flags: ignoreversion
Source: "{#MySourceRoot}/Qt6Gui.dll";     DestDir: "{app}"; Flags: ignoreversion
Source: "{#MySourceRoot}/Qt6Widgets.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MySourceRoot}/Qt6Network.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MySourceRoot}/Qt6Svg.dll";     DestDir: "{app}"; Flags: ignoreversion

; ---------------------------------------------------------------------------
;  Direct3D / 软件 OpenGL 渲染后端
; ---------------------------------------------------------------------------
Source: "{#MySourceRoot}/D3Dcompiler_47.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MySourceRoot}/opengl32sw.dll";     DestDir: "{app}"; Flags: ignoreversion

; ---------------------------------------------------------------------------
;  MinGW 运行库
; ---------------------------------------------------------------------------
Source: "{#MySourceRoot}/libgcc_s_seh-1.dll";  DestDir: "{app}"; Flags: ignoreversion
Source: "{#MySourceRoot}/libstdc++-6.dll";     DestDir: "{app}"; Flags: ignoreversion
Source: "{#MySourceRoot}/libwinpthread-1.dll"; DestDir: "{app}"; Flags: ignoreversion

; ---------------------------------------------------------------------------
;  Qt 插件子目录
; ---------------------------------------------------------------------------
Source: "{#MySourceRoot}/generic/*";            DestDir: "{app}/generic";            Flags: ignoreversion recursesubdirs
Source: "{#MySourceRoot}/iconengines/*";        DestDir: "{app}/iconengines";        Flags: ignoreversion recursesubdirs
Source: "{#MySourceRoot}/imageformats/*";       DestDir: "{app}/imageformats";       Flags: ignoreversion recursesubdirs
Source: "{#MySourceRoot}/networkinformation/*"; DestDir: "{app}/networkinformation"; Flags: ignoreversion recursesubdirs
Source: "{#MySourceRoot}/platforms/*";          DestDir: "{app}/platforms";          Flags: ignoreversion recursesubdirs
Source: "{#MySourceRoot}/styles/*";             DestDir: "{app}/styles";             Flags: ignoreversion recursesubdirs
Source: "{#MySourceRoot}/tls/*";                DestDir: "{app}/tls";                Flags: ignoreversion recursesubdirs

; ---------------------------------------------------------------------------
;  Qt 翻译文件
; ---------------------------------------------------------------------------
Source: "{#MySourceRoot}/translations/*"; DestDir: "{app}/translations"; Flags: ignoreversion recursesubdirs

[UninstallDelete]
; 清理客户端本地数据
Type: filesandordirs; Name: "{localappdata}/cloudvault/cloudvault-client"
