#pragma once

#include "..\Base\FileStream.h"
#include <vector>

void compileply(IInStream& in, FileOutputStream& a_Out);
void compileobj(IInStream& in, FileOutputStream& a_Out);
void compilemd5(IInStream& in, std::vector<IInStream*>& animFiles, FileOutputStream& a_Out);

enum e_MeshCompileType
{
	Static,
	Animated,
};

class e_MeshCompiler
{
public:
	virtual void Compile(IInStream& in, FileOutputStream& a_Out) = 0;
	virtual bool IsApplicable(const std::string& a_InputFile, IInStream& in, e_MeshCompileType* out = 0) = 0;
};

class e_ObjCompiler : public e_MeshCompiler
{
public:
	virtual void Compile(IInStream& in, FileOutputStream& a_Out);
	virtual bool IsApplicable(const std::string& a_InputFile, IInStream& in, e_MeshCompileType* out);
};

class e_Md5Compiler : public e_MeshCompiler
{
public:
	virtual void Compile(IInStream& in, FileOutputStream& a_Out);
	virtual bool IsApplicable(const std::string& a_InputFile, IInStream& in, e_MeshCompileType* out);
};

class e_PlyCompiler : public e_MeshCompiler
{
public:
	virtual void Compile(IInStream& in, FileOutputStream& a_Out);
	virtual bool IsApplicable(const std::string& a_InputFile, IInStream& in, e_MeshCompileType* out);
};

class e_MeshCompilerManager
{
private:
	std::vector<e_MeshCompiler*> m_sCompilers;
public:
	e_MeshCompilerManager()
	{
		Register(new e_ObjCompiler());
		Register(new e_Md5Compiler());
		Register(new e_PlyCompiler());
	}
	~e_MeshCompilerManager()
	{
		for(unsigned int i = 0; i < m_sCompilers.size(); i++)
			delete m_sCompilers[i];
	}
	void Compile(IInStream& in, const std::string& a_Token, FileOutputStream& a_Out, e_MeshCompileType* out = 0);
	void Register(e_MeshCompiler* C)
	{
		m_sCompilers.push_back(C);
	}
};