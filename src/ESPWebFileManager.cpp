#include "../include/ESPWebFileManager.h"
#include "../include/filemanager.h"
#include <FS.h>

String str_data = "";

bool ESPWebFileManager::begin(fs::FS& fs) {
	current_fs = &fs;
	return true; // Or add filesystem health checks
}

String ESPWebFileManager::sanitizePath(const String& path) {
	String sanitized = path;

	// Replace all instances of double slashes
	while (sanitized.indexOf("//") >= 0) {
		sanitized.replace("//", "/");
	}

	// Remove trailing slash
	if (sanitized.endsWith("/")) {
		sanitized.remove(sanitized.length() - 1);
	}

	// Ensure leading slash
	if (!sanitized.startsWith("/")) {
		sanitized = "/" + sanitized;
	}

	return sanitized;
}

bool ESPWebFileManager::safeMkdir(const String& path) {
	if (current_fs->exists(path)) return true;

	String parent = path.substring(0, path.lastIndexOf('/'));
	if (!parent.isEmpty() && !current_fs->exists(parent)) {
		if (!safeMkdir(parent)) {
			return false;
		}
	}

	return current_fs->mkdir(path);
}

//added
bool ESPWebFileManager::deleteRecursiveInternal(fs::FS& fs, const String& currentDirPath, bool isRoot) {
	File dir = current_fs->open(currentDirPath);
	if (!dir) {
		//Serial.printf("Error: Could not open directory '%s'\n", currentDirPath.c_str());
		return false;
	}
	if (!dir.isDirectory()) {
		//Serial.printf("Error: Path '%s' is not a directory.\n", currentDirPath.c_str());
		dir.close();
		return false;
	}

	dir.rewindDirectory(); // Ensure we start from the beginning

	while (true) {
		// Use a pointer to File to potentially reduce stack memory usage for each recursive call
		// The File object itself might still allocate some internal memory, but this avoids copying
		File entry = dir.openNextFile();
		if (!entry) break; // No more files

		String path = entry.name(); // entry.name() gives just the name, for the full path combine with currentDirPath
		if (!currentDirPath.endsWith("/")) {
			path = currentDirPath + "/" + path;
		}
		else {
			path = currentDirPath + path;
		}

		bool isDir = entry.isDirectory();
		entry.close(); // Close the entry file/directory handle immediately

		// Protect /config.json (only in root and if it's the specific file)
		if (isRoot && path.equalsIgnoreCase("/config.json")) {
			//Serial.printf("Skipping protected file: '%s'\n", path.c_str());
			continue; // Move to the next entry
		}

		// Skip special directories "." and ".." that can sometimes appear
		if (path.endsWith("/.") || path.endsWith("/..")) {
			continue;
		}

		if (isDir) {
			// Recursively delete contents of the subdirectory
			if (!deleteRecursiveInternal(fs, path, false)) {
				return false; // Error in subdirectory deletion
			}

			// Now remove the empty subdirectory itself
			//Serial.printf("Removing directory: '%s'\n", path.c_str());
			if (!current_fs->rmdir(path.c_str())) {
				//Serial.printf("Error removing directory: '%s'\n", path.c_str());
				// Consider adding a delay here if rmdir operations are frequent
				return false;
			}
		}
		else {
			// It's a file, remove it
			//Serial.printf("Removing file: '%s'\n", path.c_str());
			if (!current_fs->remove(path.c_str())) {
				//Serial.printf("Error removing file: '%s'\n", path.c_str());
				// Consider adding a delay here if remove operations are frequent
				return false;
			}
		}

		// IMPORTANT: Yield or delay to prevent watchdog timer timeouts
		// This is crucial for larger files or many files/folders
		delay(1); // Small delay to allow other tasks (including watchdog) to run
		// You could also use esp_task_wdt_reset() if you're managing the watchdog manually in a task,
		// but delay(1) is a simpler way for general Arduino sketches.
	}

	dir.close(); // Close the directory after processing all entries
	return true;
}

bool ESPWebFileManager::deleteRecursive(fs::FS& fs, const String& path) {
	// Protect /config.json if attempting to delete it directly
	if (path.equalsIgnoreCase("/config.json")) {
		//Serial.println("Attempted to delete protected file /config.json directly. Aborting.");
		return false;
	}

	if (!current_fs->exists(path)) {
		//Serial.printf("Path '%s' does not exist.\n", path.c_str());
		return false;
	}

	File target = current_fs->open(path);
	if (!target) {
		//Serial.printf("Error: Could not open target '%s' for deletion check.\n", path.c_str());
		return false;
	}

	bool result;
	if (target.isDirectory()) {
		target.close(); // Close the initial handle before starting recursive deletion

		// Call the internal recursive function to delete contents
		result = deleteRecursiveInternal(fs, path, path == "/");

		// After successfully deleting contents, remove the top-level directory itself
		if (result) {
			//Serial.printf("Removing top-level directory: '%s'\n", path.c_str());
			if (!current_fs->rmdir(path.c_str())) {
				//Serial.printf("Error removing top-level directory: '%s'\n", path.c_str());
				result = false;
			}
		}
	}
	else {
		// It's a single file, just remove it directly
		target.close(); // Close the initial handle
		//Serial.printf("Removing file: '%s'\n", path.c_str());
		result = current_fs->remove(path.c_str());
		if (!result) {
			//Serial.printf("Error removing file: '%s'\n", path.c_str());
		}
	}

	return result;
}

bool ESPWebFileManager::renameItem(const String& oldPath, const String& newPath) {
	String sanitizedOld = sanitizePath(oldPath);
	String sanitizedNew = sanitizePath(newPath);

	// Protect config files
	if (sanitizedOld.equalsIgnoreCase("/config.json")) {
		return false;
	}

	if (!current_fs->exists(sanitizedOld)) {
		return false;
	}

	// Check if new path already exists
	if (current_fs->exists(sanitizedNew)) {
		return false;
	}

	return current_fs->rename(sanitizedOld, sanitizedNew);
}

void ESPWebFileManager::listDir(const char* dirname, uint8_t levels) {
	File root = current_fs->open(dirname);
	if (!root || !root.isDirectory()) {
		return;
	}

	str_data = ""; // Clear the response string
	File file = root.openNextFile();

	while (file) {
		// Skip config.json in root directory
		String fileName = file.name();
		if (fileName.equalsIgnoreCase("/config.json") ||
			(dirname[0] == '/' && dirname[1] == '\0' && fileName.equalsIgnoreCase("config.json"))) {
			file = root.openNextFile();
			continue;
		}

		if (!str_data.isEmpty()) {
			str_data += ":"; // Separate entries with a colon
		}

		if (file.isDirectory()) {
			str_data += "1," + String(file.name()) + ",-"; // Folders don't have sizes
		}
		else {
			str_data += "0," + String(file.name()) + "," + String(file.size());
		}
		file = root.openNextFile();
	}

	file.close();
}

void ESPWebFileManager::listFilesRecursive(File dir, String basePath, String& output) {
	while (File entry = dir.openNextFile()) {
		String fullPath = basePath + "/" + entry.name();
		if (entry.isDirectory()) {
			output += "1," + fullPath + ":";
			listFilesRecursive(entry, fullPath, output);
		}
		else {
			output += "0," + fullPath + ":";
		}
		entry.close();
	}
}

void ESPWebFileManager::setServer(AsyncWebServer* server) {
	if (server == nullptr) {
		return;
	}
	_server = server;

	_server->on("/file", HTTP_GET, [&](AsyncWebServerRequest* request) {
		AsyncWebServerResponse* response = request->beginResponse(200, "text/html", filemanager, filemanager_len);
		response->addHeader("Content-Encoding", "gzip");
		request->send(response);
		// request->send(200, "text/html", html_page); 
		// request->send(200, "text/plain", "Test route working");
		});

	_server->on("/fm-get-fs-type", HTTP_GET, [&](AsyncWebServerRequest* request) {
		String fsTypeStr = String(1);
		request->send(200, "text/plain", fsTypeStr);
		});

	_server->on("/fm-get-folder-contents", HTTP_GET, [&](AsyncWebServerRequest* request) {
		listDir(request->arg("path").c_str(), 0);
		request->send(200, "text/plain", str_data);
		});

	// Modified upload handler to support folder uploads with directory structure
	_server->on("/fm-upload", HTTP_POST,
		[&](AsyncWebServerRequest* request) {
			request->send(200, "application/json", "{\"status\":\"success\",\"message\":\"File upload complete\"}");
		},
		[&](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
			// Get base path from query parameter
			String basePath = "/";
			if (request->hasParam("path")) {
				basePath = request->getParam("path")->value();
			}

			// Sanitize base path
			basePath = sanitizePath(basePath);
			if (!basePath.endsWith("/")) basePath += "/";

			// Process the filename which may contain relative path
			String relativePath = filename;

			// Remove any leading slashes or ./ from the relative path
			while (relativePath.startsWith("/") || relativePath.startsWith("./")) {
				if (relativePath.startsWith("/")) {
					relativePath = relativePath.substring(1);
				}
				else if (relativePath.startsWith("./")) {
					relativePath = relativePath.substring(2);
				}
			}

			// Construct full current_fstem path
			String fullPath = basePath + relativePath;

			// Create parent directories if they don't exist
			int lastSlash = fullPath.lastIndexOf('/');
			if (lastSlash > 0) {
				String parentDir = fullPath.substring(0, lastSlash);
				if (!current_fs->exists(parentDir)) {
					safeMkdir(parentDir);
				}
			}

			// Handle the file data
			if (!index) {
				// First chunk - create or truncate the file
				if (current_fs->exists(fullPath)) {
					current_fs->remove(fullPath);
				}
			}

			// Open file and write data
			File file = current_fs->open(fullPath, FILE_APPEND);
			if (!file) {
				//Serial.printf("Failed to open file for writing: %s\n", fullPath.c_str());
				return;
			}

			if (file.write(data, len) != len) {
				//Serial.printf("Write failed for file: %s\n", fullPath.c_str());
			}
			file.close();

			if (final) {
				//Serial.printf("Upload complete: %s\n", fullPath.c_str());
			}
		}
	);

	// Route to create a new folder
	_server->on("/fm-create-folder", HTTP_GET, [&](AsyncWebServerRequest* request) {
		String path = request->hasParam("path") ? request->getParam("path")->value() : "";
		path = sanitizePath(path);

		if (path.isEmpty()) {
			request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Folder path not provided\"}");
			return;
		}

		// Refresh directory cache
		String parent = path.substring(0, path.lastIndexOf('/'));
		if (!parent.isEmpty()) {
			File dir = current_fs->open(parent);
			if (dir) dir.rewindDirectory();
			dir.close();
		}

		// Use safe folder creation
		if (safeMkdir(path)) {
			request->send(200, "application/json", "{\"status\":\"success\",\"message\":\"Folder created successfully\"}");
		}
		else {
			request->send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to create folder\"}");
		}
		});

	_server->on("/fm-delete-folder", HTTP_GET, [&](AsyncWebServerRequest* request) {
		String path = request->hasParam("path") ? request->getParam("path")->value() : "";
		path = sanitizePath(path); // Sanitize the folder path

		if (path.isEmpty()) {
			request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Folder path not provided\"}");
			return;
		}

		if (current_fs->exists(path)) {
			if (ESPWebFileManager::deleteRecursive(*current_fs, path)) {
				request->send(200, "application/json", "{\"status\":\"success\",\"message\":\"Folder deleted successfully\"}");
			}
			else {
				request->send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to delete folder contents\"}");
			}
		}
		else {
			request->send(404, "application/json", "{\"status\":\"error\",\"message\":\"Folder not found\"}");
		}
		});

	_server->on("/fm-delete", HTTP_GET, [&](AsyncWebServerRequest* request) {
		String path = request->hasParam("path") ? request->getParam("path")->value() : "";
		path = sanitizePath(path); // Apply sanitization

		if (current_fs->exists(path)) {
			current_fs->remove(path);
			request->send(200, "application/json", "{\"status\":\"success\",\"message\":\"File deleted successfully\"}");
		}
		else {
			request->send(404, "application/json", "{\"status\":\"error\",\"message\":\"File not found\"}");
		}
		});

	_server->on("/fm-download", HTTP_GET, [&](AsyncWebServerRequest* request) {
		String path;
		if (request->hasParam("path")) {
			path = request->getParam("path")->value();
		}
		else {
			request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Path not provided\"}");
			return;
		}
		path = sanitizePath(path); // Apply sanitization
		if (current_fs->exists(path)) {
			request->send(*current_fs, path, String(), true);
		}
		else {
			request->send(404, "application/json", "{\"status\":\"error\",\"message\":\"File not found\"}");
		}
		});

	_server->on("/fm-rename", HTTP_GET, [&](AsyncWebServerRequest* request) {
		if (!request->hasParam("oldPath") || !request->hasParam("newPath")) {
			request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing path parameters\"}");
			return;
		}

		String oldPath = request->getParam("oldPath")->value();
		String newPath = request->getParam("newPath")->value();

		if (renameItem(oldPath, newPath)) {
			request->send(200, "application/json", "{\"status\":\"success\",\"message\":\"Item renamed successfully\"}");
		}
		else {
			request->send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to rename item\"}");
		}
		});

	_server->on("/fm-get-folder-contents-recursive", HTTP_GET, [this](AsyncWebServerRequest* request) {
    String path = request->getParam("path")->value();
    String output = "";

    File root = current_fs->open(path);  // Use current_fs pointer
    if (!root) {
        request->send(404, "text/plain", "Path not found");
        return;
    }

    listFilesRecursive(root, path, output);
    root.close();
    request->send(200, "text/plain", output);
  });
}