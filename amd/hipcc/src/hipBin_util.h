/*
Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef SRC_HIPBIN_UTIL_H_
#define SRC_HIPBIN_UTIL_H_

#include "filesystem.h"

#include "utils.h"

#include <assert.h>
#include <stdlib.h>
#include <limits.h>
#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <fstream>
#include <regex>
#include <algorithm>
#include <vector>


#if defined(_WIN32) || defined(_WIN64)
#include <tchar.h>
#include <windows.h>
#include <io.h>
#ifdef _UNICODE
  typedef wchar_t TCHAR;
  typedef std::wstring TSTR;
  typedef std::wstring::size_type TSIZE;
#define ENDLINE L"/\\"
#else
  typedef char TCHAR;
  typedef std::string TSTR;
  typedef std::string::size_type TSIZE;
#define ENDLINE "/\\"
#endif
#else
#include <unistd.h>
#endif

struct SystemCmdOut {
  std::string out;
  int exitCode = 0;
};

class HipBinUtil {
 public:
  static HipBinUtil* getInstance() {
      if (!instance)
      instance = new HipBinUtil;
      return instance;
  }
  virtual ~HipBinUtil();
  // Common helper functions
  std::string replaceStr(const std::string& s, const std::string& toReplace,
                         const std::string& replaceWith) const;
  std::string replaceRegex(const std::string& s, std::regex toReplace,
                           std::string replaceWith) const;
  SystemCmdOut exec(const char* cmd, bool printConsole) const;
  std::string getTempDir();
  void deleteTempFiles();
  std::string mktempFile(std::string name);
  std::string readConfigMap(std::map<std::string, std::string> hipVersionMap,
                            std::string keyName, std::string defaultValue) const;
  std::map<std::string, std::string> parseConfigFile(fs::path configPath) const;
  bool substringPresent(std::string fullString, std::string subString) const;
  bool stringRegexMatch(std::string fullString, std::string pattern) const;
  bool checkCmd(const std::vector<std::string>& commands, const std::string& argument);

 private:
  HipBinUtil() {}
  std::vector<std::string> tmpFiles_;
  static HipBinUtil *instance;
};

HipBinUtil *HipBinUtil::instance = 0;

// deleting temp files created
HipBinUtil::~HipBinUtil() {
  deleteTempFiles();
}

// create temp file with the template name
std::string HipBinUtil::mktempFile(std::string name) {
  std::string fileName;
#if defined(_WIN32) || defined(_WIN64)
  fileName = _mktemp(&name[0]);
#else
  fileName = mktemp(&name[0]);
#endif
  tmpFiles_.push_back(fileName);
  return fileName;
}

// matches the pattern in the string
bool HipBinUtil::stringRegexMatch(std::string fullString, std::string pattern) const {
  return std::regex_match(fullString, std::regex(pattern));
}

// subtring is present in string
bool HipBinUtil::substringPresent(std::string fullString, std::string subString) const {
  return fullString.find(subString) != std::string::npos;
}

// replaces the toReplace string with replaceWith string. Returns the new string
std::string HipBinUtil::replaceStr(const std::string& s, const std::string& toReplace,
                                   const std::string& replaceWith) const {
  std::string out = s;
  std::size_t pos = out.find(toReplace);
  if (pos == std::string::npos) return out;
  return out.replace(pos, toReplace.length(), replaceWith);
}

// replaces the toReplace regex pattern with replaceWith string.
// Returns the new string
std::string HipBinUtil::replaceRegex(const std::string& s, std::regex toReplace,
                                     std::string replaceWith) const {
  std::string out = s;
  while (std::regex_search(out, toReplace)) {
    out = std::regex_replace(out, toReplace, replaceWith);
  }
  return out;
}

// reads the config file and stores it in a map for access
std::map<std::string, std::string> HipBinUtil::parseConfigFile(fs::path configPath) const {
  std::map<std::string, std::string> configMap;
  std::ifstream isFile(configPath.string());
  std::string line;
  if (isFile.is_open()) {
    while (std::getline(isFile, line)) {
      std::istringstream is_line(line);
      std::string key;
      if (std::getline(is_line, key, '=')) {
	std::string value;
        if (std::getline(is_line, value)) {
          configMap.insert({ key, value });
        }
      }
    }
    isFile.close();
  }
  return configMap;
}

// Delete all created temporary files
void HipBinUtil::deleteTempFiles() {
  // Deleting temp files vs the temp directory
  for (unsigned int i = 0; i < tmpFiles_.size(); i++) {
    try {
      if (!fs::remove(tmpFiles_.at(i)))
        std::cerr << "Error deleting temp name: "<< tmpFiles_.at(i) << std::endl;
    }
    catch(...) {
      std::cerr << "Error deleting temp name: "<< tmpFiles_.at(i) << std::endl;
    }
  }
}

// Create a new temporary directory and return it
std::string HipBinUtil::getTempDir() {
  // mkdtemp is only applicable for unix and not windows.
  // Using filesystem becasuse of windows limitation
  std::string tmpdir = fs::temp_directory_path().string();
  // tmpDirs_.push_back(tmpdir);
  return tmpdir;
}

// executes the command, returns the status and return string
SystemCmdOut HipBinUtil::exec(const char* cmd,
                              bool printConsole = false) const {
  SystemCmdOut sysOut;
  try {
    char buffer[128];
    std::string result = "";
    #if defined(_WIN32) || defined(_WIN64)
      FILE* pipe = _popen(cmd, "r");
    #else
      FILE* pipe = popen(cmd, "r");
    #endif
    if (!pipe) throw std::runtime_error("popen() failed!");
    try {
      while (fgets(buffer, sizeof buffer, pipe) != NULL) {
        result += buffer;
      }
    } catch (...) {
      std::cerr << "Error while executing the command: " << cmd << std::endl;
    }
    #if defined(_WIN32) || defined(_WIN64)
      sysOut.exitCode = _pclose(pipe);
    #else
      int closeStatus = pclose(pipe);
      sysOut.exitCode =  WEXITSTATUS(closeStatus);
    #endif
    if (printConsole == true) {
      std::cout << result;
    }
    sysOut.out = result;
  }
  catch(...) {
    sysOut.exitCode = -1;
  }
  return sysOut;
}

// returns the value of the key from the Map passed
std::string HipBinUtil::readConfigMap(std::map<std::string, std::string> hipVersionMap,
                                      std::string keyName, std::string defaultValue) const {
  auto it = hipVersionMap.find(keyName);
  if (it != hipVersionMap.end()) {
    return it->second;
  }
  return defaultValue;
}



bool HipBinUtil::checkCmd(const std::vector<std::string>& commands,
                          const std::string& argument) {
  bool found = false;
  for (unsigned int i = 0; i < commands.size(); i++) {
    if (argument.compare(commands.at(i)) == 0) {
      found = true;
      break;
    }
  }
  return found;
}



#endif  // SRC_HIPBIN_UTIL_H_
