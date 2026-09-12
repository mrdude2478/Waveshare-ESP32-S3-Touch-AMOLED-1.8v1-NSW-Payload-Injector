#include <AsyncTCP.h>
//#include "../src/ESPAsyncWebServer/ESPAsyncWebServer.h"
#include <ESPAsyncWebServer.h>
class ESPWebFileManager {
public:
	bool begin(fs::FS& fs);
	String sanitizePath(const String& path);
	void setServer(AsyncWebServer* server);
	void listDir(const char* dirname, uint8_t levels);
	void listFilesRecursive(File dir, String basePath, String &output);
	bool deleteRecursive(fs::FS& fs, const String& path);
	bool deleteRecursiveInternal(fs::FS& fs, const String& currentDirPath, bool isRoot);
	bool safeMkdir(const String& path);
	bool renameItem(const String& oldPath, const String& newPath);

private:
	fs::FS* current_fs = nullptr;
	String str_data = "";
	AsyncWebServer* _server = nullptr;
};