# DingoBench for Windows

This portable ZIP is for Windows x64.

1. Extract the complete ZIP to a local folder you can access.
2. Keep the extracted files and subdirectories together. Run **DingoBench.exe** from that folder.
3. Open **Profiles** → **Destination profiles** and use **New** to enter a peer's Called AE title, hostname or IP address, and TCP port.
4. Select your **Source** and **Destination** in the top bar. Open **Verification** and choose **Send C-ECHO** to check a peer that supports DICOM Verification.

The package includes the runtime files used by the application and the local receive and worklist components. If launching fails because a DLL or platform plugin is missing, extract the full ZIP again instead of copying just the executable.

DingoBench supports DICOM Verification, Storage Send and Receive, Study Root query and retrieval, Modality Worklist query and serving, and local metadata inspection. **About** shows the application version. **Dependencies** shows runtime and dictionary information.

Use approved endpoints and protect received data and exported records. DICOM connections use plain TCP. Use **Stop listener** or **Stop server** when you finish with an inbound service.

The package includes the application license, third-party notices, and information identifying its corresponding source archive.
