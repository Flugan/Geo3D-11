// stdafx.h 
#pragma once

#include <direct.h>
#include "d3d11.h"
#include "log.h"
#include <vector>
#include <set>
#include <map>
#include <unordered_map>

using namespace std;

struct VSO {
	ID3D11VertexShader* Left;
	ID3D11VertexShader* Neutral;
	ID3D11VertexShader* Right;
};

map<ID3D11VertexShader*, VSO> VSOmap;

vector<byte> disassembler(vector<byte> buffer);
vector<byte> assembler(vector<byte> asmFile, vector<byte> buffer);
vector<byte> readFile(string fileName);
string shaderModel(byte* buffer);
vector<string> stringToLines(const char* start, int size);