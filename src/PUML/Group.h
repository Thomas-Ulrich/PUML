/**
 * @file
 *  This file is part of PUML
 *
 *  For conditions of distribution and use, please see the copyright
 *  notice in the file 'COPYING' at the root directory of this package
 *  and the copyright notice at https://github.com/TUM-I5/PUML
 *
 * @copyright 2013 Technische Universitaet Muenchen
 * @author Sebastian Rettenberger <rettenbs@in.tum.de>
 */

#ifndef PUML_GROUP_H
#define PUML_GROUP_H

#ifdef PARALLEL
#include <mpi.h>
#endif // PARALLEL

#include <limits>
#include <string>
#include <vector>

#include "PUML/CellType.h"
#include "PUML/Dimension.h"
#include "PUML/Entity.h"
#include "PUML/MPIElement.h"
#include "PUML/Type.h"

namespace PUML
{

/**
 * Entities are organized in groups. Each group can have multiple entities
 * but all entities in one group have the same number of elements in all
 * partitions.
 */
class Group : protected MPIElement
{
private:
	/** Name of this group */
	std::string m_name;

	/**
	 * A copy of the offset variable in this group
	 * The size of this array is one larger than numPartitions to easily
	 * compute the size of the last partition
	 */
	std::vector<size_t> m_offset;

	/** Entity the index variable */
	Entity* m_entityIndex;

public:
	Group()
		: m_entityIndex(0L)
	{
	}

	Group(const char* name, size_t numPartitions, MPIElement &comm)
		: MPIElement(comm), m_name(name), m_offset(numPartitions+1), m_entityIndex(0L)
	{
		m_offset[0] = 0;
		for (size_t i = 1; i < m_offset.size(); i++)
			m_offset[i] = std::numeric_limits<size_t>::max();
	}

	/**
	 * Constructor to for loading groups from a file
	 * Name and offsets must be set later
	 */
	Group(MPIElement &comm)
		: MPIElement(comm), m_entityIndex(0L)
	{
	}

	virtual ~Group()
	{
	}

	const char* name() const
	{
		return m_name.c_str();
	}

	/**
	 * Create a new dimension in this group
	 */
	virtual Dimension& createDimension(const char* name, size_t size) = 0;

	/**
	 * Create a new entity in this group
	 *
	 * @type The type of the entity
	 * @dimensions The dimensions of the entity (can be empty)
	 */
	virtual Entity* createEntity(const char* name, const Type &type, size_t numDimensions, Dimension* dimensions) = 0;

	/**
	 * @overload
	 */
	virtual Entity* createEntity(const char* name, const Type &type, std::vector<Dimension> &dimensions)
	{
		return createEntity(name, type, dimensions.size(), &dimensions[0]);
	}

	/**
	 * @overload
	 *
	 * Creates a one dimensional entity.
	 */
	virtual Entity* createEntity(const char* name, const Type &type)
	{
		return createEntity(name, type, 0, 0L);
	}

	/**
	 * Create to reference the vertices of a cell. Should only be used in cell groups.
	 *
	 * @param cellType The type of the cell (important for the number of vertices that belong to the cell)
	 *
	 * @see Puml::createCellGroup
	 *
	 * @ingroup HighLevelApi
	 */
	Entity* createVertexEntity(CellType cellType)
	{
		int numVertices;

		switch (cellType)
		{
		case TETRAHEDRON:
			numVertices = 4;
			break;
		}

		Dimension &dim = createDimension("vertex", numVertices);
		return createEntity("vertex", Type::Int64, 1, &dim);
	}

	virtual Entity* getEntity(const char* name) = 0;

	/**
	 * Sets the size of a partition
	 *
	 * Must be called before any entities are written to this group but after calling Pum::endDefinition.
	 * You can only set partition sizes in incrementing order.
	 *
	 * In the parallel version this is a collective function.
	 */
	bool setSize(size_t partition, size_t size)
	{
		size_t basePartition = partition;

#ifdef PARALLEL
		// Parallel mode
		// Get the size of all other partitions to compute the *offset* of the next partition
		std::vector<unsigned long> buf(2*mpiSize());
		buf[2*mpiRank()] = partition;
		buf[2*mpiRank()+1] = size;
		MPI_Allgather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL, &buf[0], 2, MPI_UNSIGNED_LONG, mpiComm());

		for (int i = 0; i < mpiSize(); i++) {
			if (buf[i*2] < basePartition)
				// Find the real base partition
				basePartition = buf[i*2];
		}
#endif // PARALLEL

		if (m_offset[basePartition] == std::numeric_limits<size_t>::max())
			// Partition size not set in incrementing order
			// We can recover from this -> file is still valid
			return false;

#ifdef PARALLEL
		// Put the sizes in the offsets (a bucket sort algorithm to sort them by partition id)
		// and then compute the real offset recursively
		for (int i = 0; i < mpiSize(); i++) {
			if (buf[2*i]+1 >= m_offset.size())
				break;

			m_offset[buf[2*i]+1] = buf[2*i+1];
		}
		for (int i = 0; i < mpiSize(); i++) {
			if (basePartition+i+1 >= m_offset.size())
				break;

			m_offset[basePartition+i+1] += m_offset[basePartition+i];
		}

#else // PARALLEL
		if (basePartition+1 < m_offset.size())
			m_offset[basePartition+1] = m_offset[basePartition] + size;
#endif // PARALLEL

		return setOffset(partition+1);
	}

	/**
	 * @return The size of a partition
	 */
	size_t size(size_t partition)
	{
		return m_offset[partition+1] - m_offset[partition];
	}

	bool putIndex(size_t partition, size_t size, const unsigned long* values)
	{
		if (m_entityIndex == 0L)
			// Not an indexed group -> do nothing
			return false;

		return m_entityIndex->put(partition, size, values);
	}

	/**
	 * Adds an index to this group
	 * Cannot be done in the constructor because of wrong values for m_parent for the indexed entity
	 *
	 * @internal
	 */
	void addIndex(size_t indexSize)
	{
		m_entityIndex = _addIndex(indexSize);
		m_entityIndex->setCollective(true);
	}

protected:
	size_t numPartitions() const
	{
		return m_offset.size()-1;
	}

	const std::vector<size_t>& offset() const
	{
		return m_offset;
	}

	std::vector<size_t>& offset()
	{
		return m_offset;
	}

	/**
	 * Write the offset of partition to file
	 */
	virtual bool setOffset(size_t partition) = 0;

	virtual Entity* _addIndex(size_t index) = 0;

	bool indexed() const
	{
		return m_entityIndex != 0L;
	}

	/**
	 * Set the index entity loaded from file
	 */
	void setEntityIndex(Entity* entityIndex)
	{
		m_entityIndex = entityIndex;
	}

	void setName(const char* name)
	{
		m_name = name;
	}

public:
	static const size_t UNLIMITED;

protected:
	static const char* DIM_PARTITION;
	static const char* DIM_SIZE;
	static const char* DIM_INDEXSIZE;

	static const char* VAR_OFFSET;
	static const char* VAR_INDEX;
};

}

#endif // PUML_GROUP_H
