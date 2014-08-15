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

#include <apfMesh2.h>

#ifndef MESH_INTPUT_H
#define MESH_INTPUT_H

/**
 * Interface for mesh input
 */
class MeshInput
{
protected:
	apf::Mesh2* m_mesh;

public:
	virtual ~MeshInput() {}

	apf::Mesh2* getMesh()
	{
		return m_mesh;
	}
};

#endif // MESH_INPUT_H
