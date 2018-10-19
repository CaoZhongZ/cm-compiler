/*
 * Copyright (c) 2017, Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
////////////////////////////////////////////////////////////////////////////////////
#include<vector>
using namespace std;

////////////////////////////////////////////////////////////////////////////
#include "CPipeline_Defs.h"
#include "cm_rt.h"

/////////////////////////////////////////////////////////////////////////////
#ifndef __CPIPELINE_H__
#define __CPIPELINE_H__

typedef unsigned char   U8;
typedef unsigned int    U32;
typedef unsigned char   uchar;
typedef unsigned short  ushort;
typedef unsigned int    uint;

#ifdef _DEBUG
#define DEBUG_printf(message, result)	printf("%s: %d\n", message, result)
#else
#define DEBUG_printf(message, result)
#endif


#define CM_Error_Handle(result, msg)	\
		if (result != CM_SUCCESS) {		\
			DEBUG_printf(msg, result);			\
			exit(1);				\
		}


//////////////////////////////////////////////////////////////////////////////////////////////
class CopyPipeline
{
public:
    ////////////////////////////////////////////////////////////////////////////////
   CopyPipeline(void);
	~CopyPipeline(void);

   //////////////////////////////////////////////////////////////////////////////
   int Init();
   int ExecuteGraph();
   int GetInputImage(char *filename);
   int SaveOutputImage();
   int AssemblerHigh6Graph();

protected:
   int AddKernel(CmKernel *pKernel);
   int GetSurface2DInfo(int width, int height, CM_SURFACE_FORMAT format,
         unsigned int * pitch, unsigned int * surface_size);
   int CreateKernel(char *isaFile, char *kernelName, CmKernel *& pKernel);
   int GetMaxThreadCount() { return m_Max_Thread_Count; }

private:
   CmDevice*    m_pCmDev;
   CmQueue      *m_pCmQueue;
   CmTask       *m_pKernelArray_Pipeline;
   CmEvent      *e_Pipeline;

   int          m_Max_Thread_Count;
   uint         m_PicWidth;
   uint         m_PicHeight;
   uchar        *m_pSrcR;
   uchar        *m_pSrcG;
   uchar        *m_pSrcB;
   uchar        *m_pDstC;
   uchar        *m_pDstM;
   uchar        *m_pDstY;
   uchar        *m_pDstK;

};
#endif //__CPIPELINE_H__
