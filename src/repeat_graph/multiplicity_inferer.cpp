//(c) 2016 by Authors
//This file is a part of ABruijn program.
//Released under the BSD license (see LICENSE file)

#include "multiplicity_inferer.h"

#include <simplex.h>
#include <variable.h>

void MultiplicityInferer::fixEdgesMultiplicity()
{
	this->estimateByCoverage();
	this->balanceGraph();
}

void MultiplicityInferer::estimateByCoverage()
{
	int64_t sumCov = 0;
	int64_t sumLength = 0;
	for (auto edge : _graph.iterEdges())
	{
		sumCov += edge->coverage * edge->length();
		sumLength += edge->length();
	}
	int meanCoverage = (sumLength != 0) ? sumCov / sumLength : 1;
	Logger::get().debug() << "Mean edge coverage: " << meanCoverage;

	for (auto edge : _graph.iterEdges())
	{
		//if (edge->isLooped() &&
		//	edge->length() < Constants::maximumJump) continue;

		float minMult = (!edge->isTip()) ? 1 : 0;
		int estMult = std::max(minMult, roundf((float)edge->coverage / 
												meanCoverage));
		edge->multiplicity = estMult;
	}
}

void MultiplicityInferer::balanceGraph()
{
	using namespace optimization;

	Logger::get().info() << "Updating edges multiplicity";

	//enumerating edges
	std::unordered_map<GraphEdge*, size_t> edgeToId;
	std::map<size_t, GraphEdge*> idToEdge;
	size_t numberEdges = 0;
	for (auto edge : _graph.iterEdges())
	{
		if (edge->isLooped()) continue;

		if (!edgeToId.count(edge))
		{
			GraphEdge* complEdge = _graph.complementPath({edge}).front();
			edgeToId[edge] = numberEdges;
			edgeToId[complEdge] = numberEdges;
			idToEdge[numberEdges] = edge;
			++numberEdges;
		}
	}

	//enumerating nodes
	std::unordered_map<GraphNode*, size_t> nodeToId;
	std::map<size_t, GraphNode*> idToNode;
	size_t numberNodes = 0;
	for (auto node : _graph.iterNodes())
	{
		if (node->inEdges.empty() || node->outEdges.empty()) continue;
		if (node->neighbors().size() < 2) continue;

		if (!nodeToId.count(node))
		{
			GraphNode* complNode = _graph.complementNode(node);
			nodeToId[complNode] = numberNodes;
			nodeToId[node] = numberNodes;
			idToNode[numberNodes] = node;
			++numberNodes;
		}
	}

	//formulate linear programming
	Simplex simplex("");
	size_t numVariables = numberEdges + numberNodes * 2;
	for (auto& idEdgePair : idToEdge)
	{
		pilal::Matrix eye(1, numVariables, 0);
		eye(idEdgePair.first) = 1;

		std::string varName = std::to_string(idEdgePair.second->edgeId.signedId());
		simplex.add_variable(new Variable(&simplex, varName.c_str()));
	
		simplex.add_constraint(Constraint(eye, CT_MORE_EQUAL, 
										  (float)idEdgePair.second->multiplicity));
		//int minMultiplicity = !edge->isTip() ? edge->multiplicity : 0;
		//int maxMultiplicity = !edge->isTip() ? 1000 : 1000;
		//simplex.add_constraint(Constraint(eye, CT_LESS_EQUAL, 1000.0f));
	}

	std::vector<std::vector<int>> incorporatedEquations;
	for (auto& idNodePair : idToNode)
	{
		size_t sourceId = numberEdges + idNodePair.first * 2;
		size_t sinkId = numberEdges + idNodePair.first * 2 + 1;

		//emergency source
		std::string sourceName = std::to_string(idNodePair.first) + "_source";
		simplex.add_variable(new Variable(&simplex, sourceName.c_str()));
		pilal::Matrix sourceMat(1, numVariables, 0);
		sourceMat(sourceId) = 1;
		simplex.add_constraint(Constraint(sourceMat, CT_MORE_EQUAL, 0.0f));

		//emergency sink
		std::string sinkName = std::to_string(idNodePair.first) + "_sink";
		simplex.add_variable(new Variable(&simplex, sinkName.c_str()));
		pilal::Matrix sinkMat(1, numVariables, 0);
		sinkMat(sinkId) = 1;
		simplex.add_constraint(Constraint(sinkMat, CT_MORE_EQUAL, 0.0f));
		
		std::vector<int> coefficients(numberEdges, 0);
		for (auto edge : idNodePair.second->inEdges) 
		{
			if (!edge->isLooped()) coefficients[edgeToId[edge]] += 1;
		}
		for (auto edge : idNodePair.second->outEdges) 
		{
			if (!edge->isLooped()) coefficients[edgeToId[edge]] -= 1;
		}

		//build the matrix with all equations and check if it's linearly independend
		pilal::Matrix problemMatrix(incorporatedEquations.size() + 1, 
									numberEdges, 0);
		for (size_t column = 0; column < incorporatedEquations.size(); ++column)
		{
			for (size_t row = 0; row < numberEdges; ++row)
			{
				problemMatrix(column, row) = incorporatedEquations[column][row];
			}
		}
		for (size_t row = 0; row < numberEdges; ++row)
		{
			problemMatrix(incorporatedEquations.size(), row) = coefficients[row];
		}
		if (!problemMatrix.rows_linearly_independent()) continue;
		//

		pilal::Matrix coefMatrix(1, numVariables, 0);
		for (size_t i = 0; i < numberEdges; ++i) 
		{
			coefMatrix(i) = coefficients[i];
		}
		coefMatrix(sourceId) = 1;
		coefMatrix(sinkId) = -1;
		simplex.add_constraint(Constraint(coefMatrix, CT_EQUAL, 0.0f));
		incorporatedEquations.push_back(std::move(coefficients));
	}

    pilal::Matrix costs(1, numVariables, 1.0f);
	for (size_t i = numberEdges; i < numVariables; ++i) costs(i) = 1000.0f;
    simplex.set_objective_function(ObjectiveFunction(OFT_MINIMIZE, costs));   

	simplex.solve();
	if (!simplex.has_solutions() || simplex.must_be_fixed() || 
		simplex.is_unlimited()) throw std::runtime_error("Error while solving LP");

	//simplex.print_solution();
	for (auto& edgeIdPair : edgeToId)
	{
		int inferredMult = simplex.get_solution()(edgeIdPair.second);
		if (edgeIdPair.first->multiplicity != inferredMult)
		{
			Logger::get().debug() << "Mult " 	
				<< edgeIdPair.first->edgeId.signedId() << " " <<
				edgeIdPair.first->multiplicity << " -> " << inferredMult;

			edgeIdPair.first->multiplicity = inferredMult;
		}
	}

	//show warning if the graph remained unbalanced
	int nodesAffected = 0;
	int sumSource = 0;
	int sumSink = 0;
	for (size_t i = 0; i < numberNodes; ++i)
	{
		int nodeSource = simplex.get_solution()(numberEdges + i * 2);
		int nodeSink = simplex.get_solution()(numberEdges + i * 2 + 1);
		sumSource += nodeSource;
		sumSink += nodeSink;
		if (nodeSource + nodeSink > 0)
		{
			++nodesAffected;
		}
	}

	if (nodesAffected)
	{
		Logger::get().warning() << "Could not balance assembly graph in full: "
			<< nodesAffected << " nodes remained, extra source: " << sumSource
			<< " extra sink: " << sumSink;
	}
}
