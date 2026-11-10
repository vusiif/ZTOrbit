; Z2 Windows installer. Compile through build-installer.ps1, which passes:
;   /DAppVersion=<VERSION>  /DSourceDir=<build/dist/Z2-windows-x64>
;   /DFileVersion=<numeric x.y.z>  /DOutputDir=<build/dist>
;   /DIconFile=<src/client/assets/app-icon.ico>
; The installer only copies the verified application directory; it never
; carries plugins, user settings or ZeroTier data.

#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef SourceDir
  #error SourceDir is required
#endif
#ifndef FileVersion
  #error FileVersion is required
#endif
#ifndef OutputDir
  #error OutputDir is required
#endif
#ifndef IconFile
  #error IconFile is required
#endif

[Setup]
; Keep AppId stable: it identifies the same product across upgrades.
AppId={{18E1FB82-CFAA-4ACC-BB7E-F4191239FB1D}
AppName=Z2
AppVersion={#AppVersion}
AppVerName=Z2 {#AppVersion}
AppPublisher=vusiif
VersionInfoVersion={#FileVersion}
VersionInfoProductName=Z2
DefaultDirName={autopf}\Z2
DefaultGroupName=Z2
DisableProgramGroupPage=yes
; Z2 itself requires administrator rights (ZeroTier's authtoken.secret).
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=Z2-setup-{#AppVersion}-windows-x64
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\Z2.exe
UninstallDisplayName=Z2
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Z2 keeps running in the tray; let Restart Manager close it for upgrades.
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Z2"; Filename: "{app}\Z2.exe"
Name: "{autodesktop}\Z2"; Filename: "{app}\Z2.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Z2.exe"; Description: "{cm:LaunchProgram,Z2}"; Flags: nowait postinstall skipifsilent
