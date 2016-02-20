/*
 * Copyright (c) 2004, Ben Supnik and Chris Serio.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "XPMPMultiplayer.h"
#include "XPMPMultiplayerVars.h"
#include "XPMPPlaneRenderer.h"
#include "XPMPMultiplayerCSL.h"

#include <algorithm>
#include <cctype>
#include <vector>
#include <string>
#include <string.h>

#include "XPLMProcessing.h"
#include "XPLMPlanes.h"
#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMPlugin.h"
#include "XPLMUtilities.h"

#include "XOGLUtils.h"
//#include "PlatformUtils.h"

#include <stdlib.h>
#include <stdio.h>
#include <set>
#include <cassert>

// This prints debug info on our process of loading Austin's planes.
#define	DEBUG_MANUAL_LOADING	0


/******************************************************************************

	T H E   T C A S   H A C K
	
 The 1.0 SDK provides no way to add TCAS blips to a panel - we simply don't know 
 where on the panel to draw.  The only way to get said blips is to manipulate
 Austin's "9 planes" plane objects, which he refers to when drawing the moving
 map. 
 
 But how do we integrate this with our system, which relies on us doing
 the drawing (either by calling Austin's low level drawing routine or just
 doing it ourselves with OpenGL)?
 
 The answer is the TCAS hack.  Basically we set Austin's number of multiplayer
 planes to zero while 3-d drawing is happening so he doesn't draw.  Then during
 2-d drawing we pop this number back up to the number of planes that are
 visible on TCAS and set the datarefs to move them, so that they appear on TCAS.

 One note: since all TCAS blips are the same, we do no model matching for 
 TCAS - we just place the first 9 planes at the right place.
 
 Our rendering loop records for us in gEnableCount how many TCAS planes should
 be visible.

 ******************************************************************************/





static	XPMPPlanePtr	XPMPPlaneIsValid(
									XPMPPlaneID 		inID, 
									XPMPPlaneVector::iterator * outIter);

// This drawing hook is called once per frame to do the real drawing.
static	int				XPMPRenderMultiplayerPlanes(
                                   XPLMDrawingPhase     inPhase,    
                                   int                  inIsBefore,    
                                   void *               inRefcon);

// This drawing hook is called twice per frame to control how many planes
// should be visible.
static	int				XPMPControlPlaneCount(
                                   XPLMDrawingPhase     inPhase,    
                                   int                  inIsBefore,    
                                   void *               inRefcon);




/********************************************************************************
 * SETUP
 ********************************************************************************/


const char * 	XPMPMultiplayerInitLegacyData(
						const char * inCSLFolder, const char * inRelatedPath,
						const char * inTexturePath, const char * inDoc8643,
						const char * inDefaultPlane,
						int (* inIntPrefsFunc)(const char *, const char *, int),
						float (* inFloatPrefsFunc)(const char *, const char *, float))
{
	gDefaultPlane = inDefaultPlane;
	gIntPrefsFunc = inIntPrefsFunc;
	gFloatPrefsFunc = inFloatPrefsFunc;

	bool	problem = false;

	if (!CSL_LoadCSL(inCSLFolder, inRelatedPath, inDoc8643))
		problem = true;

	if (!CSL_Init(inTexturePath))
		problem = true;

	if (problem)		return "There were problems initializing XSquawkBox.  Please examine X-Plane's error.out file for detailed information.";
	else 				return "";
}

const char * 	XPMPMultiplayerInit(
						int (* inIntPrefsFunc)(const char *, const char *, int),
						float (* inFloatPrefsFunc)(const char *, const char *, float))
{
	gIntPrefsFunc = inIntPrefsFunc;
	gFloatPrefsFunc = inFloatPrefsFunc;
	//char	myPath[1024];
	//char	airPath[1024];
	//char	line[256];
	//char	sysPath[1024];
	//FILE *	fi;
	
	bool	problem = false;

//	TODO - FORM GOOD DIAGNOSTIC MESSAGES HERE!
	
	// Initialize our OpenGL Utilities
	OGL_UtilsInit();

	XPMPInitDefaultPlaneRenderer();	

	// Register the plane control calls.
	XPLMRegisterDrawCallback(XPMPControlPlaneCount,
		xplm_Phase_Gauges, 0, /* after*/ 0 /* hide planes*/);	

	XPLMRegisterDrawCallback(XPMPControlPlaneCount,
		xplm_Phase_Gauges, 1, /* before */ (void *) -1 /* show planes*/);	

	// Register the actual drawing func.
	XPLMRegisterDrawCallback(XPMPRenderMultiplayerPlanes,
		xplm_Phase_Airplanes, 0, /* after*/ 0 /* refcon */);	

	if (problem)		return "There were problems initializing XSquawkBox.  Please examine X-Plane's error.out file for detailed information.";
	else 				return "";
}

void XPMPMultiplayerCleanup(void)
{
    XPLMUnregisterDrawCallback(XPMPControlPlaneCount, xplm_Phase_Gauges, 0, 0);
    XPLMUnregisterDrawCallback(XPMPControlPlaneCount, xplm_Phase_Gauges, 1, (void *) -1);
    XPLMUnregisterDrawCallback(XPMPRenderMultiplayerPlanes, xplm_Phase_Airplanes, 0, 0);
}


// We use this array to track Austin's planes, since we have to mess with them.
static	vector<string>	gPlanePaths;

const  char * XPMPMultiplayerEnable(void)
{
	// First build up a list of all of Austin's planes, and assign
	// their effective index numbers.
	gPlanePaths.clear();
	std::vector<char *>		ptrs;
	gPlanePaths.push_back("");
	
	for (size_t p = 0; p < gPackages.size(); ++p)
	{
		for (size_t pp = 0; pp < gPackages[p].planes.size(); ++pp)
		{
			if (gPackages[p].planes[pp].plane_type == plane_Austin)
			{
				gPackages[p].planes[pp].austin_idx = static_cast<int>(gPlanePaths.size());
				char	buf[1024];
				strcpy(buf,gPackages[p].planes[pp].file_path.c_str());
				#if APL
					Posix2HFSPath(buf,buf,1024);
				#endif
				gPlanePaths.push_back(buf);
			}
		}
	}
	
	// Copy the list into something that's not permanent, but is needed by the XPLM.
	for (size_t n = 0; n < gPlanePaths.size(); ++n)
	{
#if DEBUG_MANUAL_LOADING
		char	strbuf[1024];
		sprintf(strbuf, "Plane %d = '%s'\n", static_cast<int>(n), gPlanePaths[n].c_str());
		XPLMDebugString(strbuf);
#endif	
		ptrs.push_back((char *) gPlanePaths[n].c_str());
	}	
	ptrs.push_back(NULL);
	
	
	// Attempt to grab multiplayer planes, then analyze.
	int	result = XPLMAcquirePlanes(&(*ptrs.begin()), NULL, NULL);
	if (result)
		XPLMSetActiveAircraftCount(1);
	else
		XPLMDebugString("WARNING: " XPMP_CLIENT_LONGNAME " did not acquire multiplayer planes!!\n");

		int	total, 		active;
		XPLMPluginID	who;
	
	XPLMCountAircraft(&total, &active, &who);
	if (result == 0)
	{
		return "XSquawkBox was not able to start up multiplayer visuals because another plugin is controlling aircraft.";
	} else 
		return "";
}

void XPMPMultiplayerDisable(void)
{
    XPLMReleasePlanes();
}


const char * 	XPMPLoadCSLPackage(
						const char * inCSLFolder, const char * inRelatedPath, const char * inDoc8643)
{
	bool	problem = false;

	if (!CSL_LoadCSL(inCSLFolder, inRelatedPath, inDoc8643))
		problem = true;

	if (problem)		return "There were problems initializing XSquawkBox.  Please examine X-Plane's error.out file for detailed information.";
	else 				return "";
}

// This routine checks plane loading and grabs anyone we're missing.
void	XPMPLoadPlanesIfNecessary(void)
{
	int	active, models;
	XPLMPluginID	owner;
	XPLMCountAircraft(&models, &active, &owner);
	if (owner != XPLMGetMyID())
		return;
		
	if (models > static_cast<int>(gPlanePaths.size()))
		models = static_cast<int>(gPlanePaths.size());
	for (int n = 1; n < models; ++n)
	{
		if (!gPlanePaths[n].empty())
		{
			const char *	ourPath = gPlanePaths[n].c_str();
			char	realPath[512];
			char	fileName[256];
			XPLMGetNthAircraftModel(n, fileName, realPath);
			if (strcmp(ourPath, realPath))
			{
#if DEBUG_MANUAL_LOADING			
				XPLMDebugString("Manually Loading plane: ");
				XPLMDebugString(ourPath);
				XPLMDebugString("\n");
#endif				
				XPLMSetAircraftModel(n, ourPath);
			}
		}
	}

}

int XPMPGetNumberOfInstalledModels(void)
{
    size_t number = 0;
    for (const auto& package : gPackages)
    {
        number += package.planes.size();
    }
    return static_cast<int>(number);
}

void XPMPGetModelInfo(int inIndex, const char** outModelName, const char** outIcao, const char** outAirline, const char** outLivery)
{
    int counter = 0;
    for (const auto& package : gPackages)
    {

        if (counter + static_cast<int>(package.planes.size()) < inIndex + 1)
        {
            counter += static_cast<int>(package.planes.size());
            continue;
        }

        int positionInPackage =  inIndex - counter;
        *outModelName = package.planes[positionInPackage].getModelName().c_str();
        *outIcao = package.planes[positionInPackage].icao.c_str();
        *outAirline = package.planes[positionInPackage].airline.c_str();
        *outLivery = package.planes[positionInPackage].livery.c_str();
        break;
    }
}

/********************************************************************************
 * PLANE OBJECT SUPPORT
 ********************************************************************************/

XPMPPlaneID		XPMPCreatePlane(
							const char *			inICAOCode,
							const char *			inAirline,
							const char *			inLivery,
							XPMPPlaneData_f			inDataFunc,
							void *					inRefcon)
{
	XPMPPlanePtr	plane = new XPMPPlane_t;
	plane->icao = inICAOCode;
	plane->livery = inLivery;
	plane->airline = inAirline;
	plane->dataFunc = inDataFunc;
	plane->ref = inRefcon;
	plane->model = CSL_MatchPlane(inICAOCode, inAirline, inLivery, &plane->good_livery, true);
	
	plane->pos.size = sizeof(plane->pos);
	plane->surface.size = sizeof(plane->surface);
	plane->radar.size = sizeof(plane->radar);
	plane->posAge = plane->radarAge = plane->surfaceAge = -1;
	
	gPlanes.push_back(plane);
	
	for (XPMPPlaneNotifierVector::iterator iter = gObservers.begin(); iter !=
		gObservers.end(); ++iter)
	{
			iter->first.first(plane, xpmp_PlaneNotification_Created, iter->first.second);
	}
	return plane;
}

bool CompareCaseInsensitive(const string &a, const string &b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char aa, char bb) { return toupper(aa) == toupper(bb); });
}

XPMPPlaneID     XPMPCreatePlaneWithModelName(const char *inModelName, const char *inICAOCode, const char *inAirline, const char *inLivery, XPMPPlaneData_f inDataFunc, void *inRefcon)
{
    XPMPPlanePtr	plane = new XPMPPlane_t;
    plane->icao = inICAOCode;
    plane->livery = inLivery;
    plane->airline = inAirline;
    plane->dataFunc = inDataFunc;
    plane->ref = inRefcon;

    // Find the model
    for (auto &package : gPackages)
    {
        auto cslPlane = std::find_if(package.planes.begin(), package.planes.end(), [inModelName](CSLPlane_t p) { return CompareCaseInsensitive(p.getModelName(), inModelName); });
        if (cslPlane != package.planes.end())
        {
            plane->model = &(*cslPlane);
        }
    }
    if (!plane->model) return nullptr;

    plane->pos.size = sizeof(plane->pos);
    plane->surface.size = sizeof(plane->surface);
    plane->radar.size = sizeof(plane->radar);
    plane->posAge = plane->radarAge = plane->surfaceAge = -1;

    gPlanes.push_back(plane);

    for (XPMPPlaneNotifierVector::iterator iter = gObservers.begin(); iter !=
        gObservers.end(); ++iter)
    {
            iter->first.first(plane, xpmp_PlaneNotification_Created, iter->first.second);
    }
    return plane;
}

void			XPMPDestroyPlane(XPMPPlaneID inID)
{
	XPMPPlaneVector::iterator iter;
	XPMPPlanePtr plane = XPMPPlaneIsValid(inID, &iter);
	if (plane == NULL)
		return;
		
	for (XPMPPlaneNotifierVector::iterator iter2 = gObservers.begin(); iter2 !=
		gObservers.end(); ++iter2)
	{
			iter2->first.first(plane, xpmp_PlaneNotification_Destroyed, iter2->first.second);
	}
	gPlanes.erase(iter);
	
	delete plane;
}

void	XPMPChangePlaneModel(
							XPMPPlaneID				inPlaneID,
							const char *			inICAOCode,
							const char *			inAirline,
							const char *			inLivery)
{
	XPMPPlanePtr plane = XPMPPlaneIsValid(inPlaneID, NULL);
	if (plane)
	{
		plane->icao = inICAOCode;
		plane->airline = inAirline;
		plane->livery = inLivery;
		plane->model = CSL_MatchPlane(inICAOCode, inAirline, inLivery, &plane->good_livery, true);
		
	}
	
	for (XPMPPlaneNotifierVector::iterator iter2 = gObservers.begin(); iter2 !=
		gObservers.end(); ++iter2)
	{
			iter2->first.first(plane, xpmp_PlaneNotification_ModelChanged, iter2->first.second);
	}
	
}	

void	XPMPSetDefaultPlaneICAO(
							const char *			inICAO)
{
	gDefaultPlane = inICAO;
}						

long			XPMPCountPlanes(void)
{
	return static_cast<long>(gPlanes.size());
}

XPMPPlaneID		XPMPGetNthPlane(
							long 					index)
{
	if ((index < 0) || (index >= static_cast<long>(gPlanes.size())))
		return NULL;
		
	return gPlanes[index];
}							


void XPMPGetPlaneICAOAndLivery(
							XPMPPlaneID				inPlane,
							char *					outICAOCode,	// Can be NULL
							char *					outLivery)
{
	XPMPPlanePtr	plane = XPMPPlaneIsValid(inPlane, NULL);
	if (plane == NULL)
		return;
		
	if (outICAOCode)
		strcpy(outICAOCode,plane->icao.c_str());
	if (outLivery)
		strcpy(outLivery,plane->livery.c_str());
}	

void			XPMPRegisterPlaneNotifierFunc(
					XPMPPlaneNotifier_f		inFunc,
					void *					inRefcon)
{
	gObservers.push_back(XPMPPlaneNotifierTripple(XPMPPlaneNotifierPair(inFunc, inRefcon), XPLMGetMyID()));
}					

void			XPMPUnregisterPlaneNotifierFunc(
					XPMPPlaneNotifier_f		inFunc,
					void *					inRefcon)
{
	XPMPPlaneNotifierVector::iterator iter = std::find(
		gObservers.begin(), gObservers.end(), XPMPPlaneNotifierTripple(XPMPPlaneNotifierPair(inFunc, inRefcon), XPLMGetMyID()));
	if (iter != gObservers.end())
		gObservers.erase(iter);
}					

XPMPPlaneCallbackResult			XPMPGetPlaneData(
					XPMPPlaneID					inPlane,
					XPMPPlaneDataType			inDataType,
					void *						outData)
{
	XPMPPlanePtr	plane = XPMPPlaneIsValid(inPlane, NULL);
	
	XPMPPlaneCallbackResult result = xpmpData_Unavailable;
	
	if (plane == NULL)
		return result;
	
	int now = XPLMGetCycleNumber();
		
	switch(inDataType) {
	case xpmpDataType_Position:
		{
			if (plane->posAge != now)
			{
				result = plane->dataFunc(plane, inDataType, &plane->pos, plane->ref);
				if (result == xpmpData_NewData)
					plane->posAge = now;
			}
			
			XPMPPlanePosition_t *	posD = (XPMPPlanePosition_t *) outData;
			memcpy(posD, &plane->pos, XPMP_TMIN(posD->size, plane->pos.size));
			result = xpmpData_Unchanged;

			break;
		}
	case xpmpDataType_Surfaces:
		{
			if (plane->surfaceAge != now)
			{
				result = plane->dataFunc(plane, inDataType, &plane->surface, plane->ref);
				if (result == xpmpData_NewData)
					plane->surfaceAge = now;
			}
			
			XPMPPlaneSurfaces_t *	surfD = (XPMPPlaneSurfaces_t *) outData;
			memcpy(surfD, &plane->surface, XPMP_TMIN(surfD->size, plane->surface.size));
			result = xpmpData_Unchanged;

			break;
		}
	case xpmpDataType_Radar:
		{
			if (plane->radarAge != now)
			{
				result = plane->dataFunc(plane, inDataType, &plane->radar, plane->ref);
				if (result == xpmpData_NewData)
					plane->radarAge = now;
			}
			
			XPMPPlaneRadar_t *	radD = (XPMPPlaneRadar_t *) outData;
			memcpy(radD, &plane->radar, XPMP_TMIN(radD->size, plane->radar.size));
			result = xpmpData_Unchanged;

			break;
		}
	}
	return result;
}

XPMPPlanePtr	XPMPPlaneIsValid(XPMPPlaneID inID, XPMPPlaneVector::iterator * outIter)
{
	XPMPPlanePtr 	ptr = static_cast<XPMPPlanePtr>(inID);
	XPMPPlaneVector::iterator iter = std::find(gPlanes.begin(), gPlanes.end(), ptr);
	if (iter == gPlanes.end())
		return NULL;
	if (outIter)
		*outIter = iter;
	return ptr;
}

void		XPMPSetPlaneRenderer(
					XPMPRenderPlanes_f  		inRenderer, 
					void * 						inRef)
{
	gRenderer = inRenderer;
	gRendererRef = inRef;
}					

/********************************************************************************
 * RENDERING
 ********************************************************************************/

// This callback ping-pongs the multiplayer count up and back depending 
// on whether we're drawing the TCAS gauges or not.
int	XPMPControlPlaneCount(
                                   XPLMDrawingPhase     /*inPhase*/,    
                                   int                  /*inIsBefore*/,    
                                   void *               inRefcon)
{
	if (inRefcon == NULL)
	{
		XPLMSetActiveAircraftCount(1);
	} else {
		XPLMSetActiveAircraftCount(gEnableCount);
	}
	return 1;	
}


// This routine draws the actual planes.
int	XPMPRenderMultiplayerPlanes(
                                   XPLMDrawingPhase     /*inPhase*/,    
                                   int                  /*inIsBefore*/,    
                                   void *               /*inRefcon*/)
{
	static int is_blend = 0;
	
	static XPLMDataRef wrt = XPLMFindDataRef("sim/graphics/view/world_render_type");
	static XPLMDataRef prt = XPLMFindDataRef("sim/graphics/view/plane_render_type");
	
	int is_shadow = wrt != NULL && XPLMGetDatai(wrt) != 0;
	
	if(prt) 
		is_blend = XPLMGetDatai(prt) == 2;

	if (gRenderer)
		gRenderer(is_shadow ? 0 : is_blend,gRendererRef);
	else
		XPMPDefaultPlaneRenderer(is_shadow ? 0 : is_blend);
	if(!is_shadow)
	is_blend = 1 - is_blend;	
	return 1;
}

bool			XPMPIsICAOValid(
							const char *				inICAO)
 {
	return CSL_MatchPlane(inICAO, "", "", NULL, false) != NULL;
 }
 
void		XPMPDumpOneCycle(void)
{
	CSL_Dump();
	gDumpOneRenderCycle = true;
}
