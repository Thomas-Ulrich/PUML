/**
 * @file
 *  This file is part of PUML
 *
 *  For conditions of distribution and use, please see the copyright
 *  notice in the file 'COPYING' at the root directory of this package
 *  and the copyright notice at https://github.com/TUM-I5/PUML
 *
 * @copyright 2014 Technische Universitaet Muenchen
 * @author Sebastian Rettenberger <rettenbs@in.tum.de>
 */

#ifndef SIM_MOD_SUITE_H
#define SIM_MOD_SUITE_H

#include <mpi.h>

#include <algorithm>
#include <cassert>
#include <cstring>

#include <apf.h>
#include <apfMDS.h>
#include <apfMesh2.h>
#include <apfSIM.h>
#include <gmi_sim.h>

#include <SimParasolidKrnl.h>
#include <MeshSim.h>
#include <SimPartitionedMesh.h>
#include <SimError.h>
#include <SimErrorCodes.h>
#include <SimMeshingErrorCodes.h>

#include "utils/logger.h"
#include "utils/progress.h"

#include "MeshInput.h"
#include "SimModelerUtil.h"

//forward declare
pAManager SModel_attManager(pModel model);

/**
 * @todo Currently it is not supported to create more than one instance
 *  of this class
 * @todo Maybe add MS_setMaxEntities to limit the number of elements
 */
class SimModSuite : public MeshInput
{
private:
	pGModel m_model;

	pParMesh m_simMesh;

	/** Enable Simmetrix logging file */
	bool m_log;

public:
	SimModSuite(const char* modFile, const char* cadFile = 0L,
			const char* licenseFile = 0L,
			const char* meshCaseName = "mesh",
			const char* analysisCaseName = "analysis",
			int enforceSize = 0,
			const char* logFile = 0L)
	{
		// Init SimModSuite
		SimPartitionedMesh_start(0L, 0L);
		if (logFile) {
			m_log = true;
			Sim_logOn(logFile);
		} else
			m_log = false;
		Sim_readLicenseFile(licenseFile);
		MS_init();
		SimParasolid_start(1);
		Sim_setMessageHandler(messageHandler);

		// Load CAD
		logInfo(PMU_rank()) << "Loading model";
		std::string sCadFile;
		if (cadFile)
			sCadFile = cadFile;
		else {
			sCadFile = modFile;
			utils::StringUtils::replaceLast(sCadFile, ".smd", "_nat.x_t");
		}
		pNativeModel nativeModel = ParasolidNM_createFromFile(sCadFile.c_str(), 0);

		m_model = GM_load(modFile, nativeModel, 0L);
		NM_release(nativeModel);

        // check for model errors
        pPList modelErrors = PList_new();
        if (!GM_isValid(m_model, 0, modelErrors))
                // TODO print more detail about errors
                logError() << "Input model is not valid";
        PList_delete(modelErrors);

        // Extract cases
		logInfo(PMU_rank()) << "Extracting cases";
        pAManager attMngr = SModel_attManager(m_model);

		MeshingOptions meshingOptions;
		pACase meshCase = MS_newMeshCase(m_model);
		MS_setupSimModelerMeshCase(extractCase(attMngr, meshCaseName),
				meshCase, &meshingOptions);

		pACase analysisCase = extractCase(attMngr, analysisCaseName);
		pPList children = AttNode_children(analysisCase);
		void* iter = 0L;
		while (pANode child = static_cast<pANode>(PList_next(children, &iter)))
			AttCase_setModel(static_cast<pACase>(child), m_model);
		PList_delete(children);

        // create the mesh
		m_simMesh = PM_new(0, m_model, PMU_size());

        pProgress prog = Progress_new();
        Progress_setCallback(prog, progressHandler);

        logInfo(PMU_rank()) << "Starting the surface mesher";
        pSurfaceMesher surfaceMesher = SurfaceMesher_new(meshCase, m_simMesh);
        progressBar.setTotal(26);
        SurfaceMesher_execute(surfaceMesher, prog);

        logInfo(PMU_rank()) << "Starting the volume mesher";
        pVolumeMesher volumeMesher = VolumeMesher_new(meshCase, m_simMesh);
        VolumeMesher_setEnforceSize(volumeMesher, enforceSize);
        progressBar.setTotal(6);
        VolumeMesher_execute(volumeMesher, prog);

        // Cleanup mesher
        SurfaceMesher_delete(surfaceMesher);
        VolumeMesher_delete(volumeMesher);

        Progress_delete(prog);

        // Convert to APF mesh
		apf::Mesh* tmpMesh = apf::createMesh(m_simMesh);
		gmi_register_sim();
		gmi_model* model = gmi_import_sim(m_model);

		logInfo(PMU_rank()) << "Converting mesh to APF";
		m_mesh = apf::createMdsMesh(model, tmpMesh);
		apf::destroyMesh(tmpMesh);

		// Set the boundary conditions from the geometric model
		AttCase_associate(analysisCase, 0L);
		apf::MeshTag* boundaryTag = m_mesh->createIntTag("boundary condition", 1);
		apf::MeshIterator* it = m_mesh->begin(2);
		while (apf::MeshEntity* face = m_mesh->iterate(it)) {
			apf::ModelEntity* modelFace = m_mesh->toModel(face);
			if (m_mesh->getModelType(modelFace) != 2)
				continue;

			pGEntity simFace = reinterpret_cast<pGEntity>(modelFace);

			pAttribute attr = GEN_attrib(simFace, "boundaryCondition");
			if (attr) {
				char* image = Attribute_imageClass(attr);
				int boundary = parseBoundary(image);
				Sim_deleteString(image);

				m_mesh->setIntTag(face, boundaryTag, &boundary);
			}

		}
		m_mesh->end(it);
		AttCase_unassociate(analysisCase);

		// Delete cases
		MS_deleteMeshCase(meshCase);
		MS_deleteMeshCase(analysisCase);
	}

	virtual ~SimModSuite()
	{
		M_release(m_simMesh);
		// TODO we can delete the model here because it is still
		// connected to the mesh
		//GM_release(m_model);

		// Finalize SimModSuite
		SimParasolid_stop(1);
		MS_exit();
		Sim_unregisterAllKeys();
		if (m_log)
			Sim_logOff();
		SimPartitionedMesh_stop();
	}

private:
	pACase extractCase(pAManager attMngr, const char* name)
	{
		pACase acase = AMAN_findCase(attMngr, name);
		if (!acase)
			logError() << "Case" << std::string(name) << "not found.";

		AttCase_setModel(acase, m_model);

		return acase;
	}

private:

	/**
	 * @todo Make this private as soon as APF does no longer need it
	 */
	static unsigned int parseBoundary(const char* boundaryCondition)
	{
		if (strcmp(boundaryCondition, "freeSurface") == 0)
			return 1;
		if (strcmp(boundaryCondition, "dynamicRupture") == 0)
			return 3;
		if (strcmp(boundaryCondition, "absorbing") == 0)
			return 5;

		logError() << "Unknown boundary condition" << boundaryCondition;
		return -1;
	}

	static void messageHandler(int type, const char* msg)
	{
		switch (type) {
		case Sim_InfoMsg:
			// Show sim info messages as debug messages
			logDebug(PMU_rank()) << "SimModeler:" << msg;
			break;
		case Sim_DebugMsg:
			// Ignore sim debug messages
			break;
		case Sim_WarningMsg:
			logWarning(PMU_rank()) << "SimModeler:" << msg;
			break;
		case Sim_ErrorMsg:
			// Use warning because error will abort the program
			logWarning() << "SimModeler:" << msg;
			break;
		}
	}

	static void progressHandler(const char* what, int level, int startVal, int endVal, int currentVal, void *ignore)
	{
		if (PMU_rank() != 0)
			return;

		switch (level) {
		case 0:
			if (currentVal == -2)
				progressBar.update(0);
			else
				progressBar.clear();
			break;
		case 1:
			if (currentVal == 0)
				progressBar.update();
			else
				progressBar.increment();
			break;
		default:
			progressBar.update();
		}

		logDebug() << what << level << startVal << endVal << currentVal;
	}

private:
	static utils::Progress progressBar;

};

#endif // SIM_MOD_SUITE_H
