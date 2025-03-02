/*------------------------------------------------------------------------
* Vulkan Conformance Tests
* ------------------------
*
* Copyright (c) 2020 The Khronos Group Inc.
* Copyright (c) 2020 Valve Corporation.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
*//*
 * \file
 * \brief Extended dynamic state tests
*//*--------------------------------------------------------------------*/

#include "vktPipelineExtendedDynamicStateTests.hpp"
#include "vktPipelineImageUtil.hpp"
#include "vktTestCase.hpp"

#include "vkDefs.hpp"
#include "vkTypeUtil.hpp"
#include "vkQueryUtil.hpp"
#include "vkObjUtil.hpp"
#include "vkBufferWithMemory.hpp"
#include "vkImageWithMemory.hpp"
#include "vkBuilderUtil.hpp"
#include "vkCmdUtil.hpp"
#include "vkImageUtil.hpp"

#include "tcuVector.hpp"
#include "tcuMaybe.hpp"
#include "tcuTestLog.hpp"
#include "tcuVectorUtil.hpp"
#include "tcuStringTemplate.hpp"

#include "deUniquePtr.hpp"
#include "deStringUtil.hpp"

#include <vector>
#include <sstream>
#include <algorithm>
#include <utility>
#include <iterator>
#include <string>
#include <limits>
#include <memory>
#include <functional>
#include <cstddef>
#include <set>

namespace vkt
{
namespace pipeline
{

namespace
{

inline vk::VkBool32 makeVkBool32(bool value)
{
	return (value ? VK_TRUE : VK_FALSE);
}

// Framebuffer size.
constexpr deUint32	kFramebufferWidth	= 64u;
constexpr deUint32	kFramebufferHeight	= 64u;
constexpr uint32_t	kLinePointsPerRow	= 4u;
constexpr uint32_t	kLineVertexCount	= kFramebufferHeight * kLinePointsPerRow;

// Image formats.
constexpr	vk::VkFormat	kUnormColorFormat		= vk::VK_FORMAT_R8G8B8A8_UNORM;
constexpr	vk::VkFormat	kIntColorFormat			= vk::VK_FORMAT_R8G8B8A8_UINT;
const		tcu::Vec4		kUnormColorThreshold	(0.005f); // 1/255 < 0.005 < 2/255.

struct DepthStencilFormat
{
	vk::VkFormat	imageFormat;
	float			depthThreshold;
};

const DepthStencilFormat kDepthStencilFormats[] =
{
	{ vk::VK_FORMAT_D32_SFLOAT_S8_UINT,	0.0f		},
	{ vk::VK_FORMAT_D24_UNORM_S8_UINT,	1.0e-07f	},	// 1/(2**24-1) < 1.0e-07f < 2/(2**24-1)
};

using StrideVec = std::vector<vk::VkDeviceSize>;

// We will use several data types in vertex bindings. Each type will need to define a few things.
class VertexGenerator
{
public:
	// For GLSL.

	// Vertex input attribute declarations in GLSL form. One sentence per element.
	virtual std::vector<std::string>								getAttributeDeclarations()	const = 0;

	// Get statements to calculate a vec2 called "vertexCoords" using the vertex input attributes.
	virtual std::vector<std::string>								getVertexCoordCalc()		const = 0;

	// Get vertex binding declarations as part of descriptor sets, used for mesh shading.
	virtual std::vector<std::string>								getDescriptorDeclarations()	const = 0;

	// Get statements  to calculate a vec2 called "vertexCoords" using descriptor members.
	virtual std::vector<std::string>								getDescriptorCoordCalc()	const = 0;


	// For the pipeline.

	// Vertex attributes for VkPipelineVertexInputStateCreateInfo.
	virtual std::vector<vk::VkVertexInputAttributeDescription>		getAttributeDescriptions()	const = 0;

	// Vertex attributes for VK_EXT_vertex_input_dynamic_state.
	virtual std::vector<vk::VkVertexInputAttributeDescription2EXT>	getAttributeDescriptions2()	const = 0;

	// Vertex bindings for VkPipelineVertexInputStateCreateInfo.
	virtual std::vector<vk::VkVertexInputBindingDescription>		getBindingDescriptions (const StrideVec& strides) const = 0;

	// Vertex bindings for VK_EXT_vertex_input_dynamic_state.
	virtual std::vector<vk::VkVertexInputBindingDescription2EXT>	getBindingDescriptions2 (const StrideVec& strides) const = 0;

	// Create buffer data given an array of coordinates and an initial padding.
	virtual std::vector<std::vector<deUint8>>						createVertexData (const std::vector<tcu::Vec2>& coords, vk::VkDeviceSize dataOffset, vk::VkDeviceSize trailingPadding, const void* paddingPattern, size_t patternSize) const = 0;

	// Stride of vertex data in each binding.
	virtual std::vector<vk::VkDeviceSize>							getVertexDataStrides()		const = 0;
};

// Auxiliar function to create these structs more easily.
vk::VkVertexInputAttributeDescription2EXT makeVertexInputAttributeDescription2EXT (deUint32 location, deUint32 binding, vk::VkFormat format, deUint32 offset)
{
	vk::VkVertexInputAttributeDescription2EXT desc = vk::initVulkanStructure();
	desc.location = location;
	desc.binding = binding;
	desc.format = format;
	desc.offset = offset;
	return desc;
}

vk::VkVertexInputBindingDescription2EXT makeVertexInputBindingDescription2EXT (deUint32 binding, deUint32 stride, vk::VkVertexInputRate inputRate)
{
	vk::VkVertexInputBindingDescription2EXT desc = vk::initVulkanStructure();
	desc.binding = binding;
	desc.stride = stride;
	desc.inputRate = inputRate;
	desc.divisor = 1u;
	return desc;
}

// Fill a section of the given buffer (from offset to offset+count) with repeating copies of the given data.
void fillWithPattern(void* ptr_, size_t offset, size_t count, const void* src, size_t srcSize)
{
	auto	ptr		= reinterpret_cast<char*>(ptr_);
	size_t	done	= 0u;
	size_t	pending	= count;

	while (pending > 0u)
	{
		const size_t stepSize = de::min(srcSize, pending);
		deMemcpy(ptr + offset + done, src, stepSize);
		done += stepSize;
		pending -= stepSize;
	}
}

// Create a single binding vertex data vector given a type T for vertex data.
template<class T>
std::vector<deUint8> createSingleBindingVertexData (const std::vector<tcu::Vec2>& coords, vk::VkDeviceSize dataOffset, vk::VkDeviceSize trailingPadding, const void* paddingPattern, size_t patternSize)
{
	DE_ASSERT(!coords.empty());

	const auto dataOffsetSz			= static_cast<size_t>(dataOffset);
	const auto trailingPaddingSz	= static_cast<size_t>(trailingPadding);

	std::vector<deUint8> buffer;
	buffer.resize(dataOffsetSz + coords.size() * sizeof(T) + trailingPaddingSz);

	fillWithPattern(buffer.data(), 0u, dataOffsetSz, paddingPattern, patternSize);

	auto pos = dataOffsetSz;
	for (const auto& coord : coords)
	{
		new (&buffer[pos]) T(coord);
		pos += sizeof(T);
	}

	fillWithPattern(buffer.data(), pos, trailingPaddingSz, paddingPattern, patternSize);

	return buffer;
}

// Vertices in buffers will have 2 components and a padding to properly test the stride.
// This is the vertex type that will be used normally.
class VertexWithPadding : public VertexGenerator
{
protected:
	struct VertexData
	{
		VertexData(const tcu::Vec2& coords_)
			: coords	(coords_)
			, padding	(0.0f, 0.0f)
		{}

		tcu::Vec2 coords;
		tcu::Vec2 padding;
	};

public:
	virtual std::vector<std::string> getAttributeDeclarations() const override
	{
		std::vector<std::string> declarations;
		declarations.push_back("layout(location=0) in vec2 position;");
		return declarations;
	}

	virtual std::vector<std::string> getVertexCoordCalc() const override
	{
		std::vector<std::string> statements;
		statements.push_back("vec2 vertexCoords = position;");
		return statements;
	}

	virtual std::vector<std::string> getDescriptorDeclarations() const override
	{
		std::vector<std::string> declarations;
		declarations.reserve(7u);
		declarations.push_back("struct VertexData {");
		declarations.push_back("    vec2 position;");
		declarations.push_back("    vec2 padding;");
		declarations.push_back("};");
		declarations.push_back("layout(set=0, binding=0, std430) readonly buffer S0B0Block {");
		declarations.push_back("    VertexData data[];");
		declarations.push_back("} s0b0buffer;");
		return declarations;
	}

	virtual std::vector<std::string> getDescriptorCoordCalc() const override
	{
		std::vector<std::string> statements;
		statements.reserve(4u);
		statements.push_back("uint prim = uint(gl_WorkGroupID.x);");
		statements.push_back("uint indices[3] = uint[](prim, (prim + (1 + prim % 2)), (prim + (2 - prim % 2)));");
		statements.push_back("uint invIndex = indices[gl_LocalInvocationIndex];");
		statements.push_back("vec2 vertexCoords = s0b0buffer.data[invIndex].position;");
		return statements;
	}

	virtual std::vector<vk::VkVertexInputAttributeDescription> getAttributeDescriptions() const override
	{
		std::vector<vk::VkVertexInputAttributeDescription> descriptions;
		descriptions.push_back(vk::makeVertexInputAttributeDescription(0u, 0u, vk::VK_FORMAT_R32G32_SFLOAT, 0u));
		return descriptions;
	}

	// Vertex attributes for VK_EXT_vertex_input_dynamic_state.
	virtual std::vector<vk::VkVertexInputAttributeDescription2EXT> getAttributeDescriptions2() const override
	{
		std::vector<vk::VkVertexInputAttributeDescription2EXT> descriptions;
		descriptions.push_back(makeVertexInputAttributeDescription2EXT(0u, 0u, vk::VK_FORMAT_R32G32_SFLOAT, 0u));
		return descriptions;
	}

	// Vertex bindings for VkPipelineVertexInputStateCreateInfo.
	virtual std::vector<vk::VkVertexInputBindingDescription> getBindingDescriptions(const StrideVec& strides) const override
	{
		std::vector<vk::VkVertexInputBindingDescription> descriptions;
		descriptions.push_back(vk::makeVertexInputBindingDescription(0u, static_cast<deUint32>(strides.at(0)), vk::VK_VERTEX_INPUT_RATE_VERTEX));
		return descriptions;
	}

	// Vertex bindings for VK_EXT_vertex_input_dynamic_state.
	virtual std::vector<vk::VkVertexInputBindingDescription2EXT> getBindingDescriptions2(const StrideVec& strides) const override
	{
		std::vector<vk::VkVertexInputBindingDescription2EXT> descriptions;
		descriptions.push_back(makeVertexInputBindingDescription2EXT(0u, static_cast<deUint32>(strides.at(0)), vk::VK_VERTEX_INPUT_RATE_VERTEX));
		return descriptions;
	}

	virtual std::vector<std::vector<deUint8>> createVertexData (const std::vector<tcu::Vec2>& coords, vk::VkDeviceSize dataOffset, vk::VkDeviceSize trailingPadding, const void* paddingPattern, size_t patternSize) const override
	{
		return std::vector<std::vector<deUint8>>(1u, createSingleBindingVertexData<VertexData>(coords, dataOffset, trailingPadding, paddingPattern, patternSize));
	}

	virtual std::vector<vk::VkDeviceSize> getVertexDataStrides() const override
	{
		return std::vector<vk::VkDeviceSize>(1u, static_cast<vk::VkDeviceSize>(sizeof(VertexData)));
	}
};

// Vertices with coordinates, padding and an extra constant field.
class VertexWithExtraAttributes : public VertexGenerator
{
protected:
	struct VertexData
	{
		VertexData (const tcu::Vec2& coords_)
			: coords	(coords_)
			, ones		(1.0f, 1.0f)
		{
			deMemset(padding, 0, sizeof(padding));
		}

		tcu::Vec2 coords;
		tcu::Vec2 padding[10];
		tcu::Vec2 ones;
	};

public:
	virtual std::vector<std::string> getAttributeDeclarations() const override
	{
		std::vector<std::string> declarations;
		declarations.reserve(2u);
		declarations.push_back("layout(location=0) in vec2 position;");
		declarations.push_back("layout(location=1) in vec2 ones;");
		return declarations;
	}

	virtual std::vector<std::string> getVertexCoordCalc() const override
	{
		std::vector<std::string> statements;
		statements.reserve(2u);
		statements.push_back("vec2 vertexCoords = position;");
		statements.push_back("vertexCoords = vertexCoords * ones;");
		return statements;
	}

	virtual std::vector<std::string> getDescriptorDeclarations() const override
	{
		std::vector<std::string> declarations;
		declarations.reserve(8u);
		declarations.push_back("struct VertexData {");
		declarations.push_back("    vec2 coords;");
		declarations.push_back("    vec2 padding[10];");
		declarations.push_back("    vec2 ones;");
		declarations.push_back("};");
		declarations.push_back("layout(set=0, binding=0, std430) readonly buffer S0B0Block {");
		declarations.push_back("    VertexData data[];");
		declarations.push_back("} s0b0buffer;");
		return declarations;
	}

	virtual std::vector<std::string> getDescriptorCoordCalc() const override
	{
		std::vector<std::string> statements;
		statements.reserve(6u);
		statements.push_back("uint prim = uint(gl_WorkGroupID.x);");
		statements.push_back("uint indices[3] = uint[](prim, (prim + (1 + prim % 2)), (prim + (2 - prim % 2)));");
		statements.push_back("uint invIndex = indices[gl_LocalInvocationIndex];");
		statements.push_back("vec2 auxPos = s0b0buffer.data[invIndex].coords;");
		statements.push_back("vec2 auxOnes = s0b0buffer.data[invIndex].ones;");
		statements.push_back("vec2 vertexCoords = auxPos * auxOnes;");
		return statements;
	}

	virtual std::vector<vk::VkVertexInputAttributeDescription> getAttributeDescriptions() const override
	{
		std::vector<vk::VkVertexInputAttributeDescription> descriptions;
		descriptions.push_back(vk::makeVertexInputAttributeDescription(0u, 0u, vk::VK_FORMAT_R32G32_SFLOAT, 0u));
		descriptions.push_back(vk::makeVertexInputAttributeDescription(1u, 0u, vk::VK_FORMAT_R32G32_SFLOAT, static_cast<deUint32>(offsetof(VertexData, ones))));
		return descriptions;
	}

	virtual std::vector<vk::VkVertexInputAttributeDescription2EXT> getAttributeDescriptions2() const override
	{
		std::vector<vk::VkVertexInputAttributeDescription2EXT> descriptions;
		descriptions.push_back(makeVertexInputAttributeDescription2EXT(0u, 0u, vk::VK_FORMAT_R32G32_SFLOAT, 0u));
		descriptions.push_back(makeVertexInputAttributeDescription2EXT(1u, 0u, vk::VK_FORMAT_R32G32_SFLOAT, static_cast<deUint32>(offsetof(VertexData, ones))));
		return descriptions;
	}

	virtual std::vector<vk::VkVertexInputBindingDescription> getBindingDescriptions(const StrideVec& strides) const override
	{
		std::vector<vk::VkVertexInputBindingDescription> descriptions;
		descriptions.push_back(vk::makeVertexInputBindingDescription(0u, static_cast<deUint32>(strides.at(0)), vk::VK_VERTEX_INPUT_RATE_VERTEX));
		return descriptions;
	}

	virtual std::vector<vk::VkVertexInputBindingDescription2EXT> getBindingDescriptions2(const StrideVec& strides) const override
	{
		std::vector<vk::VkVertexInputBindingDescription2EXT> descriptions;
		descriptions.push_back(makeVertexInputBindingDescription2EXT(0u, static_cast<deUint32>(strides.at(0)), vk::VK_VERTEX_INPUT_RATE_VERTEX));
		return descriptions;
	}

	virtual std::vector<std::vector<deUint8>> createVertexData (const std::vector<tcu::Vec2>& coords, vk::VkDeviceSize dataOffset, vk::VkDeviceSize trailingPadding, const void* paddingPattern, size_t patternSize) const override
	{
		return std::vector<std::vector<deUint8>>(1u, createSingleBindingVertexData<VertexData>(coords, dataOffset, trailingPadding, paddingPattern, patternSize));
	}

	virtual std::vector<vk::VkDeviceSize> getVertexDataStrides() const override
	{
		return std::vector<vk::VkDeviceSize>(1u, static_cast<vk::VkDeviceSize>(sizeof(VertexData)));
	}
};

// Vertices using multiple bindings and constant fields.
// Binding 0: no data actually used.
// Binding 1: contains location 0, array of PaddingOnes.
// Binding 2: no data actually used.
// Binding 3: contains location 1, array of CoordsData.
// Binding 4: no data actually used.
// Binding 5: contains location 2, array of OneZeroPadding.
// See getAttributeDeclarations().
class MultipleBindingsVertex : public VertexGenerator
{
protected:
	struct CoordsData
	{
		tcu::Vec2 padding0;
		tcu::Vec2 coords;
		tcu::Vec2 padding1;

		CoordsData (const tcu::Vec2& coords_)
			: padding0	(0.0f, 3.0f)
			, coords	(coords_)
			, padding1	(3.0f, 0.0f)
		{}
	};

	struct PaddingOnes
	{
		tcu::Vec2 padding[4];
		tcu::Vec2 ones;

		PaddingOnes (const tcu::Vec2&)
			: ones	(1.0f, 1.0f)
		{
			deMemset(&padding, 0, sizeof(padding));
		}
	};

	struct OneZeroPadding
	{
		tcu::Vec4 oneZero;
		tcu::Vec2 padding[3];

		OneZeroPadding (const tcu::Vec2&)
			: oneZero	(1.0f, 1.0f, 0.0f, 0.0f)
		{
			deMemset(&padding, 0, sizeof(padding));
		}
	};

	struct Zeros
	{
		tcu::Vec2 zeros;

		Zeros (const tcu::Vec2&)
			: zeros	(0.0f, 0.0f)
		{}
	};

public:
	virtual std::vector<std::string> getAttributeDeclarations() const override
	{
		std::vector<std::string> declarations;
		declarations.reserve(3u);

		declarations.push_back("layout(location=0) in vec2 ones;");
		declarations.push_back("layout(location=1) in vec2 position;");
		declarations.push_back("layout(location=2) in vec4 oneZero;");

		return declarations;
	}

	virtual std::vector<std::string> getVertexCoordCalc() const override
	{
		std::vector<std::string> statements;
		statements.reserve(2u);

		statements.push_back("vec2 vertexCoords = position;");
		statements.push_back("vertexCoords = ((vertexCoords * ones) + oneZero.zw) * oneZero.xy;");

		return statements;
	}

	virtual std::vector<std::string> getDescriptorDeclarations() const override
	{
		std::vector<std::string> declarations;
		declarations.reserve(23u);

		declarations.push_back("struct PaddingOnes {");
		declarations.push_back("    vec2 padding[4];");
		declarations.push_back("    vec2 ones;");
		declarations.push_back("};");
		declarations.push_back("struct CoordsData {");
		declarations.push_back("    vec2 padding0;");
		declarations.push_back("    vec2 coords;");
		declarations.push_back("    vec2 padding1;");
		declarations.push_back("};");
		declarations.push_back("struct OneZeroPadding {");
		declarations.push_back("    vec2 ones;");		// Note: we split the vec4 into two vec2s to match CPU-side alignment.
		declarations.push_back("    vec2 zeros;");
		declarations.push_back("    vec2 padding[3];");
		declarations.push_back("};");
		declarations.push_back("layout(set=0, binding=1, std430) readonly buffer S0B1Block {");
		declarations.push_back("    PaddingOnes data[];");
		declarations.push_back("} s0b1buffer;");
		declarations.push_back("layout(set=0, binding=3, std430) readonly buffer S0B3Block {");
		declarations.push_back("    CoordsData data[];");
		declarations.push_back("} s0b3buffer;");
		declarations.push_back("layout(set=0, binding=4, std430) readonly buffer S0B5Block {");
		declarations.push_back("    OneZeroPadding data[];");
		declarations.push_back("} s0b5buffer;");

		return declarations;
	}

	virtual std::vector<std::string> getDescriptorCoordCalc() const override
	{
		std::vector<std::string> statements;
		statements.reserve(8u);
		statements.push_back("uint prim = uint(gl_WorkGroupID.x);");
		statements.push_back("uint indices[3] = uint[](prim, (prim + (1 + prim % 2)), (prim + (2 - prim % 2)));");
		statements.push_back("uint invIndex = indices[gl_LocalInvocationIndex];");
		statements.push_back("vec2 auxOnes1 = s0b1buffer.data[invIndex].ones;");
		statements.push_back("vec2 auxCoords = s0b3buffer.data[invIndex].coords;");
		statements.push_back("vec2 auxOnes5 = s0b5buffer.data[invIndex].ones;");
		statements.push_back("vec2 auxZeros = s0b5buffer.data[invIndex].zeros;");
		statements.push_back("vec2 vertexCoords = ((auxCoords * auxOnes1) + auxZeros) * auxOnes5;");
		return statements;
	}

	virtual std::vector<vk::VkVertexInputAttributeDescription> getAttributeDescriptions() const override
	{
		// We create the descriptions vector out of order to make it more interesting. See the attribute declarations.
		std::vector<vk::VkVertexInputAttributeDescription> descriptions;
		descriptions.reserve(3u);

		descriptions.push_back(vk::makeVertexInputAttributeDescription(1u, 3u, vk::VK_FORMAT_R32G32_SFLOAT, static_cast<deUint32>(offsetof(CoordsData, coords))));
		descriptions.push_back(vk::makeVertexInputAttributeDescription(2u, 5u, vk::VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<deUint32>(offsetof(OneZeroPadding, oneZero))));
		descriptions.push_back(vk::makeVertexInputAttributeDescription(0u, 1u, vk::VK_FORMAT_R32G32_SFLOAT, static_cast<deUint32>(offsetof(PaddingOnes, ones))));

		return descriptions;
	}

	virtual std::vector<vk::VkVertexInputAttributeDescription2EXT> getAttributeDescriptions2() const override
	{
		// We create the descriptions vector out of order to make it more interesting. See the attribute declarations.
		std::vector<vk::VkVertexInputAttributeDescription2EXT> descriptions;
		descriptions.reserve(3u);

		descriptions.push_back(makeVertexInputAttributeDescription2EXT(2u, 5u, vk::VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<deUint32>(offsetof(OneZeroPadding, oneZero))));
		descriptions.push_back(makeVertexInputAttributeDescription2EXT(1u, 3u, vk::VK_FORMAT_R32G32_SFLOAT, static_cast<deUint32>(offsetof(CoordsData, coords))));
		descriptions.push_back(makeVertexInputAttributeDescription2EXT(0u, 1u, vk::VK_FORMAT_R32G32_SFLOAT, static_cast<deUint32>(offsetof(PaddingOnes, ones))));

		return descriptions;
	}

	virtual std::vector<vk::VkVertexInputBindingDescription> getBindingDescriptions(const StrideVec& strides) const override
	{
		// Provide descriptions out of order to make it more interesting.
		std::vector<vk::VkVertexInputBindingDescription> descriptions;
		descriptions.reserve(6u);

		descriptions.push_back(vk::makeVertexInputBindingDescription(2u, static_cast<deUint32>(strides.at(2)), vk::VK_VERTEX_INPUT_RATE_INSTANCE));
		descriptions.push_back(vk::makeVertexInputBindingDescription(0u, static_cast<deUint32>(strides.at(0)), vk::VK_VERTEX_INPUT_RATE_INSTANCE));
		descriptions.push_back(vk::makeVertexInputBindingDescription(1u, static_cast<deUint32>(strides.at(1)), vk::VK_VERTEX_INPUT_RATE_VERTEX));
		descriptions.push_back(vk::makeVertexInputBindingDescription(4u, static_cast<deUint32>(strides.at(4)), vk::VK_VERTEX_INPUT_RATE_INSTANCE));
		descriptions.push_back(vk::makeVertexInputBindingDescription(3u, static_cast<deUint32>(strides.at(3)), vk::VK_VERTEX_INPUT_RATE_VERTEX));
		descriptions.push_back(vk::makeVertexInputBindingDescription(5u, static_cast<deUint32>(strides.at(5)), vk::VK_VERTEX_INPUT_RATE_VERTEX));

		return descriptions;
	}

	virtual std::vector<vk::VkVertexInputBindingDescription2EXT> getBindingDescriptions2(const StrideVec& strides) const override
	{
		// Provide descriptions out of order to make it more interesting.
		std::vector<vk::VkVertexInputBindingDescription2EXT> descriptions;
		descriptions.reserve(6u);

		descriptions.push_back(makeVertexInputBindingDescription2EXT(2u, static_cast<deUint32>(strides.at(2)), vk::VK_VERTEX_INPUT_RATE_INSTANCE));
		descriptions.push_back(makeVertexInputBindingDescription2EXT(0u, static_cast<deUint32>(strides.at(0)), vk::VK_VERTEX_INPUT_RATE_INSTANCE));
		descriptions.push_back(makeVertexInputBindingDescription2EXT(1u, static_cast<deUint32>(strides.at(1)), vk::VK_VERTEX_INPUT_RATE_VERTEX));
		descriptions.push_back(makeVertexInputBindingDescription2EXT(5u, static_cast<deUint32>(strides.at(5)), vk::VK_VERTEX_INPUT_RATE_VERTEX));
		descriptions.push_back(makeVertexInputBindingDescription2EXT(4u, static_cast<deUint32>(strides.at(4)), vk::VK_VERTEX_INPUT_RATE_INSTANCE));
		descriptions.push_back(makeVertexInputBindingDescription2EXT(3u, static_cast<deUint32>(strides.at(3)), vk::VK_VERTEX_INPUT_RATE_VERTEX));

		return descriptions;
	}

	virtual std::vector<std::vector<deUint8>> createVertexData (const std::vector<tcu::Vec2>& coords, vk::VkDeviceSize dataOffset, vk::VkDeviceSize trailingPadding, const void* paddingPattern, size_t patternSize) const override
	{
		std::vector<std::vector<deUint8>> result;
		result.reserve(6u);

		result.push_back(createSingleBindingVertexData<Zeros>(coords, dataOffset, trailingPadding, paddingPattern, patternSize));			// Not actually used.
		result.push_back(createSingleBindingVertexData<PaddingOnes>(coords, dataOffset, trailingPadding, paddingPattern, patternSize));		// Binding 1 contains location=0 as PaddingOnes.
		result.push_back(createSingleBindingVertexData<Zeros>(coords, dataOffset, trailingPadding, paddingPattern, patternSize));			// Not actually used.
		result.push_back(createSingleBindingVertexData<CoordsData>(coords, dataOffset, trailingPadding, paddingPattern, patternSize));		// Binding 3 contains location=1 as CoordsData.
		result.push_back(createSingleBindingVertexData<Zeros>(coords, dataOffset, trailingPadding, paddingPattern, patternSize));			// Not actually used.
		result.push_back(createSingleBindingVertexData<OneZeroPadding>(coords, dataOffset, trailingPadding, paddingPattern, patternSize));	// Binding 5 contains location=2 as OneZeroPadding.

		return result;
	}

	virtual std::vector<vk::VkDeviceSize> getVertexDataStrides() const override
	{
		std::vector<vk::VkDeviceSize> strides;
		strides.reserve(6u);

		strides.push_back(static_cast<vk::VkDeviceSize>(sizeof(Zeros)));
		strides.push_back(static_cast<vk::VkDeviceSize>(sizeof(PaddingOnes)));
		strides.push_back(static_cast<vk::VkDeviceSize>(sizeof(Zeros)));
		strides.push_back(static_cast<vk::VkDeviceSize>(sizeof(CoordsData)));
		strides.push_back(static_cast<vk::VkDeviceSize>(sizeof(Zeros)));
		strides.push_back(static_cast<vk::VkDeviceSize>(sizeof(OneZeroPadding)));

		return strides;
	}
};

// Stencil Operation parameters, as used in vkCmdSetStencilOpEXT().
struct StencilOpParams
{
	vk::VkStencilFaceFlags  faceMask;
	vk::VkStencilOp         failOp;
	vk::VkStencilOp         passOp;
	vk::VkStencilOp         depthFailOp;
	vk::VkCompareOp         compareOp;
};

const StencilOpParams kDefaultStencilOpParams =
{
	vk::VK_STENCIL_FACE_FRONT_AND_BACK,
	vk::VK_STENCIL_OP_KEEP,
	vk::VK_STENCIL_OP_KEEP,
	vk::VK_STENCIL_OP_KEEP,
	vk::VK_COMPARE_OP_ALWAYS
};

struct DepthBiasParams
{
	float constantFactor;
	float clamp;
};

const DepthBiasParams kNoDepthBiasParams = { 0.0f, 0.0f };

using ViewportVec	= std::vector<vk::VkViewport>;
using ScissorVec	= std::vector<vk::VkRect2D>;
using StencilOpVec	= std::vector<StencilOpParams>;

// Generic, to be used with any state than can be set statically and, as an option, dynamically.
template<typename T>
struct StaticAndDynamicPair
{
	T				staticValue;
	tcu::Maybe<T>	dynamicValue;

	// Helper constructor to set a static value and no dynamic value.
	StaticAndDynamicPair (const T& value)
		: staticValue	(value)
		, dynamicValue	(tcu::Nothing)
	{
	}

	// Helper constructor to set both.
	StaticAndDynamicPair (const T& sVal, const T& dVal)
		: staticValue	(sVal)
		, dynamicValue	(tcu::just<T>(dVal))
	{
	}

	// If the dynamic value is present, swap static and dynamic values.
	void swapValues (void)
	{
		if (!dynamicValue)
			return;
		std::swap(staticValue, dynamicValue.get());
	}
};

// For anything boolean, see below.
using BooleanFlagConfig = StaticAndDynamicPair<bool>;

// Configuration for every aspect of the extended dynamic state.
using CullModeConfig				= StaticAndDynamicPair<vk::VkCullModeFlags>;
using FrontFaceConfig				= StaticAndDynamicPair<vk::VkFrontFace>;
using TopologyConfig				= StaticAndDynamicPair<vk::VkPrimitiveTopology>;
using ViewportConfig				= StaticAndDynamicPair<ViewportVec>;	// At least one element.
using ScissorConfig					= StaticAndDynamicPair<ScissorVec>;		// At least one element.
using StrideConfig					= StaticAndDynamicPair<StrideVec>;		// At least one element.
using DepthTestEnableConfig			= BooleanFlagConfig;
using DepthWriteEnableConfig		= BooleanFlagConfig;
using DepthCompareOpConfig			= StaticAndDynamicPair<vk::VkCompareOp>;
using DepthBoundsTestEnableConfig	= BooleanFlagConfig;
using StencilTestEnableConfig		= BooleanFlagConfig;
using StencilOpConfig				= StaticAndDynamicPair<StencilOpVec>;	// At least one element.
using VertexGeneratorConfig			= StaticAndDynamicPair<const VertexGenerator*>;
using DepthBiasEnableConfig			= BooleanFlagConfig;
using RastDiscardEnableConfig		= BooleanFlagConfig;
using PrimRestartEnableConfig		= BooleanFlagConfig;
using LogicOpConfig					= StaticAndDynamicPair<vk::VkLogicOp>;
using PatchControlPointsConfig		= StaticAndDynamicPair<deUint8>;
using DepthBiasConfig				= StaticAndDynamicPair<DepthBiasParams>;

const tcu::Vec4		kDefaultTriangleColor	(0.0f, 0.0f, 1.0f, 1.0f);	// Opaque blue.
const tcu::Vec4		kDefaultClearColor		(0.0f, 0.0f, 0.0f, 1.0f);	// Opaque black.

const tcu::Vec4		kLogicOpTriangleColor	(0.0f, 0.0f,255.f,255.f);	// Opaque blue. Note: tcu::Vec4 and will be cast to the appropriate type in the shader.
const tcu::UVec4	kGreenClearColor		(  0u, 255u,   0u, 255u);	// Opaque green, UINT.
const tcu::UVec4	kLogicOpFinalColor		(  0u, 255u, 255u, 255u);	// Opaque cyan, UINT.

struct MeshParams
{
	tcu::Vec4	color;
	float		depth;
	bool		reversed;
	float		scaleX;
	float		scaleY;
	float		offsetX;
	float		offsetY;
	float		stripScale;

	MeshParams (const tcu::Vec4&	color_		= kDefaultTriangleColor,
				float				depth_		= 0.0f,
				bool				reversed_	= false,
				float				scaleX_		= 1.0f,
				float				scaleY_		= 1.0f,
				float				offsetX_	= 0.0f,
				float				offsetY_	= 0.0f,
				float				stripScale_	= 0.0f)
		: color			(color_)
		, depth			(depth_)
		, reversed		(reversed_)
		, scaleX		(scaleX_)
		, scaleY		(scaleY_)
		, offsetX		(offsetX_)
		, offsetY		(offsetY_)
		, stripScale	(stripScale_)
	{}
};

enum class SequenceOrdering
{
	CMD_BUFFER_START	= 0,	// Set state at the start of the command buffer.
	BEFORE_DRAW			= 1,	// After binding dynamic pipeline and just before drawing.
	BETWEEN_PIPELINES	= 2,	// After a static state pipeline has been bound but before the dynamic state pipeline has been bound.
	AFTER_PIPELINES		= 3,	// After a static state pipeline and a second dynamic state pipeline have been bound.
	BEFORE_GOOD_STATIC	= 4,	// Before a static state pipeline with the correct values has been bound.
	TWO_DRAWS_DYNAMIC	= 5,	// Bind bad static pipeline and draw, followed by binding correct dynamic pipeline and drawing again.
	TWO_DRAWS_STATIC	= 6,	// Bind bad dynamic pipeline and draw, followed by binding correct static pipeline and drawing again.
};

class ReferenceColorGenerator
{
public:
	typedef std::unique_ptr<ReferenceColorGenerator> P;

	virtual void	operator()	(tcu::PixelBufferAccess&)	const = 0;
	virtual P		clone		()							const = 0;
};

// Most tests expect a single output color in the whole image.
class SingleColorGenerator : public ReferenceColorGenerator
{
public:
	SingleColorGenerator (const tcu::Vec4& color)
		: m_colorFloat	(color)
		, m_colorUint	(0u)
		, isUint		(false)
	{}

	SingleColorGenerator (const tcu::UVec4& color)
		: m_colorFloat	(0.0f)
		, m_colorUint	(color)
		, isUint		(true)
	{}

	void operator()(tcu::PixelBufferAccess& access) const override
	{
		constexpr auto kWidth	= static_cast<int>(kFramebufferWidth);
		constexpr auto kHeight	= static_cast<int>(kFramebufferHeight);

		for (int y = 0; y < kHeight; ++y)
		for (int x = 0; x < kWidth; ++x)
		{
			if (isUint)
				access.setPixel(m_colorUint, x, y);
			else
				access.setPixel(m_colorFloat, x, y);
		}
	}

	P clone() const override
	{
		return P(new SingleColorGenerator(*this));
	}

private:
	const tcu::Vec4		m_colorFloat;
	const tcu::UVec4	m_colorUint;
	const bool			isUint;
};

// Some tests expect the upper half and the lower half having different color values.
class HorizontalSplitGenerator : public ReferenceColorGenerator
{
public:
	HorizontalSplitGenerator (const tcu::Vec4& top, const tcu::Vec4& bottom)
		: m_top(top), m_bottom(bottom)
	{}

	void operator()(tcu::PixelBufferAccess& access) const override
	{
		constexpr auto kWidth		= static_cast<int>(kFramebufferWidth);
		constexpr auto kHeight		= static_cast<int>(kFramebufferHeight);
		constexpr auto kHalfHeight	= kHeight / 2;

		for (int y = 0; y < kHeight; ++y)
		for (int x = 0; x < kWidth; ++x)
		{
			const auto& color = (y < kHalfHeight ? m_top : m_bottom);
			access.setPixel(color, x, y);
		}
	}

	P clone() const override
	{
		return P(new HorizontalSplitGenerator(*this));
	}

private:
	const tcu::Vec4 m_top;
	const tcu::Vec4 m_bottom;
};

// Primitive restart tests expect the last line to have some missing pixels.
class LastSegmentMissingGenerator : public ReferenceColorGenerator
{
public:
	LastSegmentMissingGenerator (const tcu::Vec4& geomColor, const tcu::Vec4& clearColor)
		: m_geomColor	(geomColor)
		, m_clearColor	(clearColor)
	{}

	void operator()(tcu::PixelBufferAccess& access) const override
	{
		constexpr auto kWidth				= static_cast<int>(kFramebufferWidth);
		constexpr auto kHeight				= static_cast<int>(kFramebufferHeight);
		constexpr auto kLastSegmentStart	= static_cast<int>(kWidth * 0.75f);

		for (int y = 0; y < kHeight; ++y)
		for (int x = 0; x < kWidth; ++x)
		{
			// The last segment of the last line has the background color.
			const auto& color = ((y == kHeight - 1 && x >= kLastSegmentStart) ? m_clearColor : m_geomColor);
			access.setPixel(color, x, y);
		}
	}

	P clone() const override
	{
		return P(new LastSegmentMissingGenerator(*this));
	}

private:
	const tcu::Vec4 m_geomColor;
	const tcu::Vec4 m_clearColor;
};

const VertexGenerator* getVertexWithPaddingGenerator ()
{
	static VertexWithPadding vertexWithPadding;
	return &vertexWithPadding;
}

const VertexGenerator* getVertexWithExtraAttributesGenerator ()
{
	static VertexWithExtraAttributes vertexWithExtraAttributes;
	return &vertexWithExtraAttributes;
}

const VertexGenerator* getVertexWithMultipleBindingsGenerator ()
{
	static MultipleBindingsVertex multipleBindingsVertex;
	return &multipleBindingsVertex;
}

// Create VertexGeneratorConfig varying constructor depending on having none, only the static or both.
VertexGeneratorConfig makeVertexGeneratorConfig (const VertexGenerator* staticGen, const VertexGenerator* dynamicGen)
{
	DE_ASSERT(!(dynamicGen && !staticGen));
	if (dynamicGen)
		return VertexGeneratorConfig(staticGen, dynamicGen);
	if (staticGen)
		return VertexGeneratorConfig(staticGen);
	return VertexGeneratorConfig(getVertexWithPaddingGenerator());	// Only static part with a default option.
}

// Similar to makeVertexGeneratorConfig, choosing the final value.
const VertexGenerator* chooseVertexGenerator (const VertexGenerator* staticGen, const VertexGenerator* dynamicGen)
{
	DE_ASSERT(!(dynamicGen && !staticGen));
	if (dynamicGen)
		return dynamicGen;
	if (staticGen)
		return staticGen;
	return getVertexWithPaddingGenerator();
}

enum class TopologyClass
{
	POINT,
	LINE,
	TRIANGLE,
	PATCH,
	INVALID,
};

std::string topologyClassName (TopologyClass tclass)
{
	switch (tclass)
	{
	case TopologyClass::POINT:		return "point";
	case TopologyClass::LINE:		return "line";
	case TopologyClass::TRIANGLE:	return "triangle";
	case TopologyClass::PATCH:		return "patch";
	default:
		break;
	}

	DE_ASSERT(false);
	return "";
}

#ifndef CTS_USES_VULKANSC
// Is a particular dynamic state incompatible with mesh shading pipelines?
bool isMeshShadingPipelineIncompatible (vk::VkDynamicState state)
{
	switch (state)
	{
	case vk::VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT:
	case vk::VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT:
	case vk::VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE_EXT:
	case vk::VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT:
	case vk::VK_DYNAMIC_STATE_VERTEX_INPUT_EXT:
		return true;
	default:
		return false;
	}

	// Unreachable.
	DE_ASSERT(false);
	return false;
}

// Is a particular dynamic state compatible with mesh shading pipelines?
bool isMeshShadingPipelineCompatible (vk::VkDynamicState state)
{
	return !isMeshShadingPipelineIncompatible(state);
}
#endif // CTS_USES_VULKANSC

TopologyClass getTopologyClass (vk::VkPrimitiveTopology topology)
{
	switch (topology)
	{
	case vk::VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
		return TopologyClass::POINT;
	case vk::VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
	case vk::VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
	case vk::VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
	case vk::VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
		return TopologyClass::LINE;
	case vk::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
	case vk::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
	case vk::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
	case vk::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
	case vk::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
		return TopologyClass::TRIANGLE;
	case vk::VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
		return TopologyClass::PATCH;
	default:
		break;
	}

	DE_ASSERT(false);
	return TopologyClass::INVALID;
}

struct TestConfig
{
	// Should we use pipeline_library to construct pipeline.
	vk::PipelineConstructionType	pipelineConstructionType;

	// Main sequence ordering.
	SequenceOrdering				sequenceOrdering;

	// Drawing parameters: tests will draw one or more flat meshes of triangles covering the whole "screen".
	std::vector<MeshParams>			meshParams;			// Mesh parameters for each full-screen layer of geometry.
	deUint32						referenceStencil;	// Reference stencil value.

	// Clearing parameters for the framebuffer.
	vk::VkClearValue				clearColorValue;
	float							clearDepthValue;
	deUint32						clearStencilValue;

	// Expected output in the attachments.
	ReferenceColorGenerator::P		referenceColor;
	float							expectedDepth;
	deUint32						expectedStencil;

	// Depth bounds parameters for the pipeline.
	float							minDepthBounds;
	float							maxDepthBounds;

	// Force inclusion of passthrough geometry shader or not.
	bool							forceGeometryShader;

	// Use mesh shaders instead of classic pipelines.
	bool							useMeshShaders;

	// Bind an unused mesh shading pipeline before binding the dynamic pipeline.
	// This will only be used in the CMD_BUFFER_START sequence ordering, to minimize the number of cases.
	bool							bindUnusedMeshShadingPipeline;

	// Force single vertex in the VBO.
	bool							singleVertex;
	deUint32						singleVertexDrawCount;

	// Offset and extra room after the vertex buffer data.
	vk::VkDeviceSize				vertexDataOffset;
	vk::VkDeviceSize				vertexDataExtraBytes;

	// Bind and draw with a pipeline that uses dynamic patch control points but doesn't actually use a tessellation
	// shader, before using the real pipelines being tested.
	bool							useExtraDynPCPPipeline;

	// Static and dynamic pipeline configuration.
	VertexGeneratorConfig			vertexGenerator;
	CullModeConfig					cullModeConfig;
	FrontFaceConfig					frontFaceConfig;
	TopologyConfig					topologyConfig;
	ViewportConfig					viewportConfig;
	ScissorConfig					scissorConfig;
	StrideConfig					strideConfig;
	DepthTestEnableConfig			depthTestEnableConfig;
	DepthWriteEnableConfig			depthWriteEnableConfig;
	DepthCompareOpConfig			depthCompareOpConfig;
	DepthBoundsTestEnableConfig		depthBoundsTestEnableConfig;
	StencilTestEnableConfig			stencilTestEnableConfig;
	StencilOpConfig					stencilOpConfig;
	DepthBiasEnableConfig			depthBiasEnableConfig;
	RastDiscardEnableConfig			rastDiscardEnableConfig;
	PrimRestartEnableConfig			primRestartEnableConfig;
	LogicOpConfig					logicOpConfig;
	PatchControlPointsConfig		patchControlPointsConfig;
	DepthBiasConfig					depthBiasConfig;

	// Sane defaults.
	TestConfig (vk::PipelineConstructionType pipelineType, SequenceOrdering ordering, bool useMeshShaders_, const VertexGenerator* staticVertexGenerator = nullptr, const VertexGenerator* dynamicVertexGenerator = nullptr)
		: pipelineConstructionType		(pipelineType)
		, sequenceOrdering				(ordering)
		, meshParams					(1u, MeshParams())
		, referenceStencil				(0u)
		, clearColorValue				(vk::makeClearValueColor(kDefaultClearColor))
		, clearDepthValue				(1.0f)
		, clearStencilValue				(0u)
		, referenceColor				(new SingleColorGenerator(kDefaultTriangleColor))
		, expectedDepth					(1.0f)
		, expectedStencil				(0u)
		, minDepthBounds				(0.0f)
		, maxDepthBounds				(1.0f)
		, forceGeometryShader			(false)
		, useMeshShaders				(useMeshShaders_)
		, bindUnusedMeshShadingPipeline	(false)
		, singleVertex					(false)
		, singleVertexDrawCount			(0)
		, vertexDataOffset				(0ull)
		, vertexDataExtraBytes			(0ull)
		, useExtraDynPCPPipeline		(false)
		, vertexGenerator				(makeVertexGeneratorConfig(staticVertexGenerator, dynamicVertexGenerator))
		, cullModeConfig				(static_cast<vk::VkCullModeFlags>(vk::VK_CULL_MODE_NONE))
		, frontFaceConfig				(vk::VK_FRONT_FACE_COUNTER_CLOCKWISE)
		// By default we will use a triangle strip with 6 vertices that could be wrongly interpreted as a triangle list with 2 triangles.
		, topologyConfig				(vk::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
		, viewportConfig				(ViewportVec(1u, vk::makeViewport(kFramebufferWidth, kFramebufferHeight)))
		, scissorConfig					(ScissorVec(1u, vk::makeRect2D(kFramebufferWidth, kFramebufferHeight)))
		// By default, the vertex stride is the size of a vertex according to the chosen vertex type.
		, strideConfig					(chooseVertexGenerator(staticVertexGenerator, dynamicVertexGenerator)->getVertexDataStrides())
		, depthTestEnableConfig			(false)
		, depthWriteEnableConfig		(false)
		, depthCompareOpConfig			(vk::VK_COMPARE_OP_NEVER)
		, depthBoundsTestEnableConfig	(false)
		, stencilTestEnableConfig		(false)
		, stencilOpConfig				(StencilOpVec(1u, kDefaultStencilOpParams))
		, depthBiasEnableConfig			(false)
		, rastDiscardEnableConfig		(false)
		, primRestartEnableConfig		(false)
		, logicOpConfig					(vk::VK_LOGIC_OP_CLEAR)
		, patchControlPointsConfig		(1u)
		, depthBiasConfig				(kNoDepthBiasParams)
		, m_swappedValues				(false)
	{
	}

	TestConfig (const TestConfig& other)
		: pipelineConstructionType		(other.pipelineConstructionType)
		, sequenceOrdering				(other.sequenceOrdering)
		, meshParams					(other.meshParams)
		, referenceStencil				(other.referenceStencil)
		, clearColorValue				(other.clearColorValue)
		, clearDepthValue				(other.clearDepthValue)
		, clearStencilValue				(other.clearStencilValue)
		, referenceColor				(other.referenceColor->clone())
		, expectedDepth					(other.expectedDepth)
		, expectedStencil				(other.expectedStencil)
		, minDepthBounds				(other.minDepthBounds)
		, maxDepthBounds				(other.maxDepthBounds)
		, forceGeometryShader			(other.forceGeometryShader)
		, useMeshShaders				(other.useMeshShaders)
		, bindUnusedMeshShadingPipeline	(other.bindUnusedMeshShadingPipeline)
		, singleVertex					(other.singleVertex)
		, singleVertexDrawCount			(other.singleVertexDrawCount)
		, vertexDataOffset				(other.vertexDataOffset)
		, vertexDataExtraBytes			(other.vertexDataExtraBytes)
		, useExtraDynPCPPipeline		(other.useExtraDynPCPPipeline)
		, vertexGenerator				(other.vertexGenerator)
		, cullModeConfig				(other.cullModeConfig)
		, frontFaceConfig				(other.frontFaceConfig)
		, topologyConfig				(other.topologyConfig)
		, viewportConfig				(other.viewportConfig)
		, scissorConfig					(other.scissorConfig)
		, strideConfig					(other.strideConfig)
		, depthTestEnableConfig			(other.depthTestEnableConfig)
		, depthWriteEnableConfig		(other.depthWriteEnableConfig)
		, depthCompareOpConfig			(other.depthCompareOpConfig)
		, depthBoundsTestEnableConfig	(other.depthBoundsTestEnableConfig)
		, stencilTestEnableConfig		(other.stencilTestEnableConfig)
		, stencilOpConfig				(other.stencilOpConfig)
		, depthBiasEnableConfig			(other.depthBiasEnableConfig)
		, rastDiscardEnableConfig		(other.rastDiscardEnableConfig)
		, primRestartEnableConfig		(other.primRestartEnableConfig)
		, logicOpConfig					(other.logicOpConfig)
		, patchControlPointsConfig		(other.patchControlPointsConfig)
		, depthBiasConfig				(other.depthBiasConfig)
		, m_swappedValues				(other.m_swappedValues)
	{
	}

	// Get the proper viewport vector according to the test config.
	const ViewportVec& getActiveViewportVec () const
	{
		return ((viewportConfig.dynamicValue && !m_swappedValues) ? viewportConfig.dynamicValue.get() : viewportConfig.staticValue);
	}

	// Gets the proper vertex generator according to the test config.
	const VertexGenerator* getActiveVertexGenerator () const
	{
		return ((vertexGenerator.dynamicValue && !m_swappedValues) ? vertexGenerator.dynamicValue.get() : vertexGenerator.staticValue);
	}

	// Gets the inactive vertex generator according to the test config. If there's only one, return that.
	const VertexGenerator* getInactiveVertexGenerator () const
	{
		return ((vertexGenerator.dynamicValue && m_swappedValues) ? vertexGenerator.dynamicValue.get() : vertexGenerator.staticValue);
	}

	// Get the active number of patch control points according to the test config.
	deUint32 getActivePatchControlPoints () const
	{
		return ((patchControlPointsConfig.dynamicValue && !m_swappedValues) ? patchControlPointsConfig.dynamicValue.get() : patchControlPointsConfig.staticValue);
	}

	// Get the active depth bias parameters.
	DepthBiasParams getActiveDepthBiasParams () const
	{
		return ((depthBiasConfig.dynamicValue && !m_swappedValues) ? depthBiasConfig.dynamicValue.get() : depthBiasConfig.staticValue);
	}

	// Get the active primitive restart enable value.
	bool getActivePrimitiveRestartEnable () const
	{
		return ((primRestartEnableConfig.dynamicValue && !m_swappedValues) ? primRestartEnableConfig.dynamicValue.get() : primRestartEnableConfig.staticValue);
	}

	// Returns true if there is more than one viewport.
	bool isMultiViewport () const
	{
		return (getActiveViewportVec().size() > 1);
	}

	// Returns true if the case needs a geometry shader.
	bool needsGeometryShader () const
	{
		// Writing to gl_ViewportIndex from vertex or tesselation shaders needs the shaderOutputViewportIndex feature, which is less
		// commonly supported than geometry shaders, so we will use a geometry shader if we need to write to it.
		return ((isMultiViewport() && (!useMeshShaders)) || forceGeometryShader);
	}

	// Returns true if we should use the static and dynamic values exchanged.
	// This makes the static part of the pipeline have the actual expected values.
	bool isReversed () const
	{
		return (sequenceOrdering == SequenceOrdering::BEFORE_GOOD_STATIC ||
				sequenceOrdering == SequenceOrdering::TWO_DRAWS_STATIC);
	}

	// Swaps static and dynamic configuration values.
	void swapValues ()
	{
		vertexGenerator.swapValues();
		cullModeConfig.swapValues();
		frontFaceConfig.swapValues();
		topologyConfig.swapValues();
		viewportConfig.swapValues();
		scissorConfig.swapValues();
		strideConfig.swapValues();
		depthTestEnableConfig.swapValues();
		depthWriteEnableConfig.swapValues();
		depthCompareOpConfig.swapValues();
		depthBoundsTestEnableConfig.swapValues();
		stencilTestEnableConfig.swapValues();
		stencilOpConfig.swapValues();
		depthBiasEnableConfig.swapValues();
		rastDiscardEnableConfig.swapValues();
		primRestartEnableConfig.swapValues();
		logicOpConfig.swapValues();
		patchControlPointsConfig.swapValues();
		depthBiasConfig.swapValues();

		m_swappedValues = !m_swappedValues;
	}

	// Returns the number of iterations when recording commands.
	deUint32 numIterations () const
	{
		deUint32 iterations = 0u;

		switch (sequenceOrdering)
		{
		case SequenceOrdering::TWO_DRAWS_DYNAMIC:
		case SequenceOrdering::TWO_DRAWS_STATIC:
			iterations = 2u;
			break;
		default:
			iterations = 1u;
			break;
		}

		return iterations;
	}

	// Returns true if we're testing the logic op.
	bool testLogicOp () const
	{
		return static_cast<bool>(logicOpConfig.dynamicValue);
	}

	// Returns true if we're testing the patch control points.
	bool testPatchControlPoints () const
	{
		return static_cast<bool>(patchControlPointsConfig.dynamicValue);
	}

	// Returns true if the topology class is patches for tessellation.
	bool patchesTopology () const
	{
		return (getTopologyClass(topologyConfig.staticValue) == TopologyClass::PATCH);
	}

	// Returns true if the test needs tessellation shaders.
	bool needsTessellation () const
	{
		return (testPatchControlPoints() || patchesTopology());
	}

	// Returns true if the test needs an index buffer.
	bool needsIndexBuffer () const
	{
		// When checking a dynamic values for primitive restart enable, we will use 8-bit indices and line drawing, which allows us
		// to hit vertex index 255 with the last vertex. Line mode uses 4 points per row of pixels and 64 rows in the image.
		if (static_cast<bool>(primRestartEnableConfig.dynamicValue))
		{
			DE_ASSERT(getTopologyClass(topologyConfig.staticValue) == TopologyClass::LINE);
			DE_ASSERT(kLineVertexCount == static_cast<uint32_t>(std::numeric_limits<uint8_t>::max()) + 1u);
			return true;
		}
		return false;
	}

	// Returns true if the test needs the depth bias clamp feature.
	bool needsDepthBiasClampFeature () const
	{
		return (getActiveDepthBiasParams().clamp != 0.0f);
	}

	// Returns the appropriate color image format for the test.
	vk::VkFormat colorFormat () const
	{
		// Pick int color format when testing logic op.
		return (testLogicOp() ? kIntColorFormat : kUnormColorFormat);
	}

	// Returns the list of dynamic states affected by this config.
	std::vector<vk::VkDynamicState> getDynamicStates () const
	{
		std::vector<vk::VkDynamicState> dynamicStates;

		if (depthBiasConfig.dynamicValue)				dynamicStates.push_back(vk::VK_DYNAMIC_STATE_DEPTH_BIAS);
		if (cullModeConfig.dynamicValue)				dynamicStates.push_back(vk::VK_DYNAMIC_STATE_CULL_MODE_EXT);
		if (frontFaceConfig.dynamicValue)				dynamicStates.push_back(vk::VK_DYNAMIC_STATE_FRONT_FACE_EXT);
		if (topologyConfig.dynamicValue)				dynamicStates.push_back(vk::VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT);
		if (viewportConfig.dynamicValue)				dynamicStates.push_back(vk::VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT_EXT);
		if (scissorConfig.dynamicValue)					dynamicStates.push_back(vk::VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT_EXT);
		if (strideConfig.dynamicValue)					dynamicStates.push_back(vk::VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT);
		if (depthTestEnableConfig.dynamicValue)			dynamicStates.push_back(vk::VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT);
		if (depthWriteEnableConfig.dynamicValue)		dynamicStates.push_back(vk::VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT);
		if (depthCompareOpConfig.dynamicValue)			dynamicStates.push_back(vk::VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT);
		if (depthBoundsTestEnableConfig.dynamicValue)	dynamicStates.push_back(vk::VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE_EXT);
		if (stencilTestEnableConfig.dynamicValue)		dynamicStates.push_back(vk::VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT);
		if (stencilOpConfig.dynamicValue)				dynamicStates.push_back(vk::VK_DYNAMIC_STATE_STENCIL_OP_EXT);
		if (vertexGenerator.dynamicValue)				dynamicStates.push_back(vk::VK_DYNAMIC_STATE_VERTEX_INPUT_EXT);
		if (patchControlPointsConfig.dynamicValue)		dynamicStates.push_back(vk::VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT);
		if (rastDiscardEnableConfig.dynamicValue)		dynamicStates.push_back(vk::VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE_EXT);
		if (depthBiasEnableConfig.dynamicValue)			dynamicStates.push_back(vk::VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT);
		if (logicOpConfig.dynamicValue)					dynamicStates.push_back(vk::VK_DYNAMIC_STATE_LOGIC_OP_EXT);
		if (primRestartEnableConfig.dynamicValue)		dynamicStates.push_back(vk::VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE_EXT);

		return dynamicStates;
	}

#ifndef CTS_USES_VULKANSC
	// Returns true if the test configuration uses dynamic states which are incompatible with mesh shading pipelines.
	bool badMeshShadingPipelineDynState () const
	{
		const auto states = getDynamicStates();
		return std::any_of(begin(states), end(states), isMeshShadingPipelineIncompatible);
	}
#endif // CTS_USES_VULKANSC

	bool testEDS() const
	{
		return (cullModeConfig.dynamicValue
			|| frontFaceConfig.dynamicValue
			|| topologyConfig.dynamicValue
			|| viewportConfig.dynamicValue
			|| scissorConfig.dynamicValue
			|| strideConfig.dynamicValue
			|| depthTestEnableConfig.dynamicValue
			|| depthWriteEnableConfig.dynamicValue
			|| depthCompareOpConfig.dynamicValue
			|| depthBoundsTestEnableConfig.dynamicValue
			|| stencilTestEnableConfig.dynamicValue
			|| stencilOpConfig.dynamicValue);
	}

	bool testEDS2() const
	{
		return (rastDiscardEnableConfig.dynamicValue
			|| depthBiasEnableConfig.dynamicValue
			|| primRestartEnableConfig.dynamicValue
			|| useExtraDynPCPPipeline);
	}

	bool testVertexDynamic() const
	{
		return static_cast<bool>(vertexGenerator.dynamicValue);
	}

private:
	// Extended dynamic state cases as created by createExtendedDynamicStateTests() are based on the assumption that, when a state
	// has a static and a dynamic value configured at the same time, the static value is wrong and the dynamic value will give
	// expected results. That's appropriate for most test variants, but in some others we want to reverse the situation: a dynamic
	// pipeline with wrong values and a static one with good values.
	//
	// Instead of modifying how tests are created, we use isReversed() and swapValues() above, allowing us to swap static and
	// dynamic values and to know if we should do it for a given test case. However, we need to know were the good value is at any
	// given point in time in order to correctly answer some questions while running the test. m_swappedValues tracks that state.
	bool m_swappedValues;
};

struct PushConstants
{
	tcu::Vec4	triangleColor;
	float		meshDepth;
	deInt32		viewPortIndex;
	float		scaleX;
	float		scaleY;
	float		offsetX;
	float		offsetY;
	float		stripScale;
};

void copy(vk::VkStencilOpState& dst, const StencilOpParams& src)
{
	dst.failOp		= src.failOp;
	dst.passOp		= src.passOp;
	dst.depthFailOp	= src.depthFailOp;
	dst.compareOp	= src.compareOp;
}

class ExtendedDynamicStateTest : public vkt::TestCase
{
public:
							ExtendedDynamicStateTest		(tcu::TestContext& testCtx, const std::string& name, const std::string& description, const TestConfig& testConfig);
	virtual					~ExtendedDynamicStateTest		(void) {}

	virtual void			checkSupport					(Context& context) const;
	virtual void			initPrograms					(vk::SourceCollections& programCollection) const;
	virtual TestInstance*	createInstance					(Context& context) const;

private:
	TestConfig				m_testConfig;
};

class ExtendedDynamicStateInstance : public vkt::TestInstance
{
public:
								ExtendedDynamicStateInstance	(Context& context, const TestConfig& testConfig);
	virtual						~ExtendedDynamicStateInstance	(void) {}

	virtual tcu::TestStatus		iterate							(void);

private:
	TestConfig					m_testConfig;
};

ExtendedDynamicStateTest::ExtendedDynamicStateTest (tcu::TestContext& testCtx, const std::string& name, const std::string& description, const TestConfig& testConfig)
	: vkt::TestCase	(testCtx, name, description)
	, m_testConfig	(testConfig)
{
	const auto staticTopologyClass = getTopologyClass(testConfig.topologyConfig.staticValue);
	DE_UNREF(staticTopologyClass); // For release builds.

	// Matching topology classes.
	DE_ASSERT(!testConfig.topologyConfig.dynamicValue ||
			  staticTopologyClass == getTopologyClass(testConfig.topologyConfig.dynamicValue.get()));

	// Supported topology classes for these tests.
	DE_ASSERT(staticTopologyClass == TopologyClass::LINE || staticTopologyClass == TopologyClass::TRIANGLE
		|| staticTopologyClass == TopologyClass::PATCH);

	// Make sure these are consistent.
	DE_ASSERT(!(m_testConfig.testPatchControlPoints() && !m_testConfig.patchesTopology()));
	DE_ASSERT(!(m_testConfig.patchesTopology() && m_testConfig.getActivePatchControlPoints() <= 1u));

	// Do not use an extra dynamic patch control points pipeline if we're not testing them.
	DE_ASSERT(!m_testConfig.useExtraDynPCPPipeline || m_testConfig.testPatchControlPoints());
}

void ExtendedDynamicStateTest::checkSupport (Context& context) const
{
	const auto&	vki				= context.getInstanceInterface();
	const auto	physicalDevice	= context.getPhysicalDevice();

	// Check feature support.
	const auto& edsFeatures		= context.getExtendedDynamicStateFeaturesEXT();
	const auto& eds2Features	= context.getExtendedDynamicState2FeaturesEXT();
	const auto& viFeatures		= context.getVertexInputDynamicStateFeaturesEXT();
#ifndef CTS_USES_VULKANSC
	const auto& meshFeatures	= context.getMeshShaderFeaturesEXT();
#endif // CTS_USES_VULKANSC

	if (m_testConfig.testEDS() && !edsFeatures.extendedDynamicState)
		TCU_THROW(NotSupportedError, "extendedDynamicState is not supported");

	if (m_testConfig.testEDS2() && !eds2Features.extendedDynamicState2)
		TCU_THROW(NotSupportedError, "extendedDynamicState2 is not supported");

	if (m_testConfig.testLogicOp() && !eds2Features.extendedDynamicState2LogicOp)
		TCU_THROW(NotSupportedError, "extendedDynamicState2LogicOp is not supported");

	if ((m_testConfig.testPatchControlPoints() || m_testConfig.useExtraDynPCPPipeline) && !eds2Features.extendedDynamicState2PatchControlPoints)
		TCU_THROW(NotSupportedError, "extendedDynamicState2PatchControlPoints is not supported");

	if (m_testConfig.testVertexDynamic() && !viFeatures.vertexInputDynamicState)
		TCU_THROW(NotSupportedError, "vertexInputDynamicState is not supported");

#ifndef CTS_USES_VULKANSC
	if ((m_testConfig.useMeshShaders || m_testConfig.bindUnusedMeshShadingPipeline) && !meshFeatures.meshShader)
		TCU_THROW(NotSupportedError, "meshShader is not supported");
#endif // CTS_USES_VULKANSC

	// Check the number of viewports needed and the corresponding limits.
	const auto&	viewportConfig	= m_testConfig.viewportConfig;
	auto		numViewports	= viewportConfig.staticValue.size();

	if (viewportConfig.dynamicValue)
		numViewports = std::max(numViewports, viewportConfig.dynamicValue.get().size());

	if (numViewports > 1)
	{
		const auto properties = vk::getPhysicalDeviceProperties(vki, physicalDevice);
		if (numViewports > static_cast<decltype(numViewports)>(properties.limits.maxViewports))
			TCU_THROW(NotSupportedError, "Number of viewports not supported (" + de::toString(numViewports) + ")");
	}

	const auto&	dbTestEnable	= m_testConfig.depthBoundsTestEnableConfig;
	const bool	useDepthBounds	= (dbTestEnable.staticValue || (dbTestEnable.dynamicValue && dbTestEnable.dynamicValue.get()));

	if (useDepthBounds || m_testConfig.needsGeometryShader() || m_testConfig.needsTessellation() || m_testConfig.needsDepthBiasClampFeature())
	{
		const auto features = vk::getPhysicalDeviceFeatures(vki, physicalDevice);

		// Check depth bounds test support.
		if (useDepthBounds && !features.depthBounds)
			TCU_THROW(NotSupportedError, "Depth bounds feature not supported");

		// Check geometry shader support.
		if (m_testConfig.needsGeometryShader() && !features.geometryShader)
			TCU_THROW(NotSupportedError, "Geometry shader not supported");

		// Check tessellation support
		if (m_testConfig.needsTessellation() && !features.tessellationShader)
			TCU_THROW(NotSupportedError, "Tessellation feature not supported");

		// Check depth bias clamp feature.
		if (m_testConfig.needsDepthBiasClampFeature() && !features.depthBiasClamp)
			TCU_THROW(NotSupportedError, "Depth bias clamp not supported");
	}

	// Check color image format support (depth/stencil will be chosen at runtime).
	const vk::VkFormatFeatureFlags	kColorFeatures	= (vk::VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | vk::VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);

	// Pick int color format for logic op
	vk::VkFormat					colorFormat		= m_testConfig.colorFormat();
	const auto						colorProperties	= vk::getPhysicalDeviceFormatProperties(vki, physicalDevice, colorFormat);

	if ((colorProperties.optimalTilingFeatures & kColorFeatures) != kColorFeatures)
		TCU_THROW(NotSupportedError, "Required color image features not supported");

	checkPipelineLibraryRequirements(vki, physicalDevice, m_testConfig.pipelineConstructionType);
}

void ExtendedDynamicStateTest::initPrograms (vk::SourceCollections& programCollection) const
{
	const vk::ShaderBuildOptions meshBuildOptions (programCollection.usedVulkanVersion, vk::SPIRV_VERSION_1_4, 0u, true);

	std::ostringstream pushSource;
	std::ostringstream vertSourceTemplateStream;
	std::ostringstream fragSource;
	std::ostringstream geomSource;
	std::ostringstream tescSource;
	std::ostringstream teseSource;
	std::ostringstream meshSource;

	pushSource
		<< "layout(push_constant, std430) uniform PushConstantsBlock {\n"
		<< "    vec4  triangleColor;\n"
		<< "    float depthValue;\n"
		<< "    int   viewPortIndex;\n"
		<< "    float scaleX;\n"
		<< "    float scaleY;\n"
		<< "    float offsetX;\n"
		<< "    float offsetY;\n"
		<< "    float stripScale;\n"
		<< "} pushConstants;\n"
		;
	const auto pushConstants = pushSource.str();

	// The actual generator, attributes and calculations.
	const auto			activeGen	= m_testConfig.getActiveVertexGenerator();
	const auto			attribDecls	= activeGen->getAttributeDeclarations();
	const auto			coordCalcs	= activeGen->getVertexCoordCalc();
	const auto			descDeclsV	= activeGen->getDescriptorDeclarations();
	const auto			descCalcsV	= activeGen->getDescriptorCoordCalc();

	// The static generator, attributes and calculations, for the static pipeline, if needed.
	const auto			inactiveGen		= m_testConfig.getInactiveVertexGenerator();
	const auto			staticAttribDec	= inactiveGen->getAttributeDeclarations();
	const auto			staticCoordCalc	= inactiveGen->getVertexCoordCalc();

	std::ostringstream	activeAttribs;
	std::ostringstream	activeCalcs;
	std::ostringstream	inactiveAttribs;
	std::ostringstream	inactiveCalcs;
	std::ostringstream	descDecls;
	std::ostringstream	descCalcs;

	for (const auto& decl : attribDecls)
		activeAttribs << decl << "\n";

	for (const auto& statement : coordCalcs)
		activeCalcs << "    " << statement << "\n";

	for (const auto& decl : staticAttribDec)
		inactiveAttribs << decl << "\n";

	for (const auto& statement : staticCoordCalc)
		inactiveCalcs << "    " << statement << "\n";

	for (const auto& decl : descDeclsV)
		descDecls << decl << "\n";

	for (const auto& calc : descCalcsV)
		descCalcs << "    " << calc << "\n";

	vertSourceTemplateStream
		<< "#version 450\n"
		<< pushConstants
		<< "${ATTRIBUTES}"
		<< "out gl_PerVertex\n"
		<< "{\n"
		<< "    vec4 gl_Position;\n"
		<< "};\n"
		<< "void main() {\n"
		<< "${CALCULATIONS}"
		<< "    gl_Position = vec4(vertexCoords.x * pushConstants.scaleX + pushConstants.offsetX, vertexCoords.y * pushConstants.scaleY + pushConstants.offsetY, pushConstants.depthValue, 1.0);\n"
		<< "    vec2 stripOffset;\n"
		<< "    switch (gl_VertexIndex) {\n"
		<< "    case 0: stripOffset = vec2(0.0, 0.0); break;\n"
		<< "    case 1: stripOffset = vec2(0.0, 1.0); break;\n"
		<< "    case 2: stripOffset = vec2(1.0, 0.0); break;\n"
		<< "    case 3: stripOffset = vec2(1.0, 1.0); break;\n"
		<< "    case 4: stripOffset = vec2(2.0, 0.0); break;\n"
		<< "    case 5: stripOffset = vec2(2.0, 1.0); break;\n"
		<< "    default: stripOffset = vec2(-1000.0); break;\n"
		<< "    }\n"
		<< "    gl_Position.xy += pushConstants.stripScale * stripOffset;\n"
		<< "}\n"
		;

	tcu::StringTemplate vertSourceTemplate (vertSourceTemplateStream.str());

	std::map<std::string, std::string> activeMap;
	std::map<std::string, std::string> inactiveMap;

	activeMap["ATTRIBUTES"]		= activeAttribs.str();
	activeMap["CALCULATIONS"]	= activeCalcs.str();

	inactiveMap["ATTRIBUTES"]	= inactiveAttribs.str();
	inactiveMap["CALCULATIONS"]	= inactiveCalcs.str();

	const auto activeVertSource		= vertSourceTemplate.specialize(activeMap);
	const auto inactiveVertSource	= vertSourceTemplate.specialize(inactiveMap);

	const auto colorFormat	= m_testConfig.colorFormat();
	const auto vecType		= (vk::isUnormFormat(colorFormat) ? "vec4" : "uvec4");

	fragSource
		<< "#version 450\n"
		<< pushConstants
		<< "layout(location=0) out " << vecType << " color;\n"
		<< "void main() {\n"
		<< "    color = " << vecType << "(pushConstants.triangleColor);\n"
		<< "}\n"
		;

	if (m_testConfig.needsGeometryShader())
	{
		const auto			topologyClass	= getTopologyClass(m_testConfig.topologyConfig.staticValue);
		const std::string	inputPrimitive	= ((topologyClass == TopologyClass::LINE) ? "lines" : "triangles");
		const deUint32		vertexCount		= ((topologyClass == TopologyClass::LINE) ? 2u : 3u);
		const std::string	outputPrimitive	= ((topologyClass == TopologyClass::LINE) ? "line_strip" : "triangle_strip");

		geomSource
			<< "#version 450\n"
			<< "layout (" << inputPrimitive << ") in;\n"
			<< "layout (" << outputPrimitive << ", max_vertices=" << vertexCount << ") out;\n"
			<< (m_testConfig.isMultiViewport() ? pushConstants : "")
			<< "in gl_PerVertex\n"
			<< "{\n"
			<< "    vec4 gl_Position;\n"
			<< "} gl_in[" << vertexCount << "];\n"
			<< "out gl_PerVertex\n"
			<< "{\n"
			<< "    vec4 gl_Position;\n"
			<< "};\n"
			<< "void main() {\n"
			<< (m_testConfig.isMultiViewport() ? "    gl_ViewportIndex = pushConstants.viewPortIndex;\n" : "")
			;

		for (deUint32 i = 0; i < vertexCount; ++i)
		{
			geomSource
				<< "    gl_Position = gl_in[" << i << "].gl_Position;\n"
				<< "    EmitVertex();\n"
				;
		}

		geomSource
			<< "}\n"
			;
	}

	if (m_testConfig.needsTessellation())
	{
		tescSource
			<< "#version 450\n"
			<< "#extension GL_EXT_tessellation_shader : require\n"
			<< "layout(vertices=3) out;\n"
			<< "in gl_PerVertex\n"
			<< "{\n"
			<< "    vec4 gl_Position;\n"
			<< "} gl_in[gl_MaxPatchVertices];\n"
			<< "out gl_PerVertex\n"
			<< "{\n"
			<< "  vec4 gl_Position;\n"
			<< "} gl_out[];\n"
			<< "void main() {\n"
			<< "  gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
			<< "  gl_TessLevelOuter[0] = 3.0;\n"
			<< "  gl_TessLevelOuter[1] = 3.0;\n"
			<< "  gl_TessLevelOuter[2] = 3.0;\n"
			<< "  gl_TessLevelInner[0] = 3.0;\n"
			<< "}\n"
			;
		teseSource
			<< "#version 450\n"
			<< "#extension GL_EXT_tessellation_shader : require\n"
			<< "layout(triangles) in;\n"
			<< "in gl_PerVertex\n"
			<< "{\n"
			<< "  vec4 gl_Position;\n"
			<< "} gl_in[gl_MaxPatchVertices];\n"
			<< "out gl_PerVertex\n"
			<< "{\n"
			<< "  vec4 gl_Position;\n"
			<< "};\n"
			<< "void main() {\n"
			<< "  gl_Position = (gl_in[0].gl_Position * gl_TessCoord.x + \n"
			<< "                 gl_in[1].gl_Position * gl_TessCoord.y + \n"
			<< "                 gl_in[2].gl_Position * gl_TessCoord.z);\n"
			<< "}\n";
	}

#ifndef CTS_USES_VULKANSC
	if (m_testConfig.useMeshShaders)
	{
		DE_ASSERT(!m_testConfig.needsGeometryShader());
		DE_ASSERT(!m_testConfig.needsTessellation());
		DE_ASSERT(!m_testConfig.needsIndexBuffer());

		// Make sure no dynamic states incompatible with mesh shading pipelines are used.
		DE_ASSERT(!m_testConfig.badMeshShadingPipelineDynState());

		// Shader below is designed to work with vertex buffers containing triangle strips as used by default.
		DE_ASSERT(m_testConfig.topologyConfig.staticValue == vk::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
		DE_ASSERT(!m_testConfig.singleVertex);

		meshSource
			<< "#version 450\n"
			<< "#extension GL_EXT_mesh_shader : enable\n"
			<< "layout(local_size_x=3, local_size_y=1, local_size_z=1) in;\n"
			<< "layout(triangles) out;\n"
			<< "layout(max_vertices=3, max_primitives=1) out;\n"
			<< pushConstants
			<< (m_testConfig.isMultiViewport()
				? "perprimitiveEXT out gl_MeshPerPrimitiveEXT { int gl_ViewportIndex; } gl_MeshPrimitivesEXT[];\n"
				: "")
			<< descDecls.str()
			<< "void main() {\n"
			<< descCalcs.str()
			<< "    SetMeshOutputsEXT(3u, 1u);\n"
			<< "    gl_MeshVerticesEXT[gl_LocalInvocationIndex].gl_Position = vec4(vertexCoords.x * pushConstants.scaleX + pushConstants.offsetX, vertexCoords.y * pushConstants.scaleY + pushConstants.offsetY, pushConstants.depthValue, 1.0);\n"
			<< "    if (gl_LocalInvocationIndex == 0u) {\n"
			<< "        gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);\n"
			<< (m_testConfig.isMultiViewport()
				? "        gl_MeshPrimitivesEXT[0].gl_ViewportIndex = pushConstants.viewPortIndex;\n"
				: "")
			<< "    }\n"
			<< "}\n"
			;
	}
#endif // CTS_USES_VULKANSC

	// In reversed test configurations, the pipeline with dynamic state needs to have the inactive shader.
	const auto kReversed = m_testConfig.isReversed();
	programCollection.glslSources.add("dynamicVert") << glu::VertexSource(kReversed ? inactiveVertSource : activeVertSource);
	programCollection.glslSources.add("staticVert") << glu::VertexSource(kReversed ? activeVertSource : inactiveVertSource);

	programCollection.glslSources.add("frag") << glu::FragmentSource(fragSource.str());
	if (m_testConfig.needsGeometryShader())
		programCollection.glslSources.add("geom") << glu::GeometrySource(geomSource.str());
	if (m_testConfig.needsTessellation())
	{
		programCollection.glslSources.add("tesc") << glu::TessellationControlSource(tescSource.str());
		programCollection.glslSources.add("tese") << glu::TessellationEvaluationSource(teseSource.str());
	}
	if (m_testConfig.useMeshShaders)
		programCollection.glslSources.add("mesh") << glu::MeshSource(meshSource.str()) << meshBuildOptions;

	if (m_testConfig.bindUnusedMeshShadingPipeline)
	{
		std::ostringstream meshNoOut;
		meshNoOut
			<< "#version 450\n"
			<< "#extension GL_EXT_mesh_shader : enable\n"
			<< "layout(local_size_x=1, local_size_y=1, local_size_z=1) in;\n"
			<< "layout(triangles) out;\n"
			<< "layout(max_vertices=3, max_primitives=1) out;\n"
			<< "void main() {\n"
			<< "    SetMeshOutputsEXT(0u, 0u);\n"
			<< "}\n"
			;
		programCollection.glslSources.add("meshNoOut") << glu::MeshSource(meshNoOut.str()) << meshBuildOptions;
	}

	// Extra vert and frag shaders for the extra patch control points pipeline. These draw offscreen.
	if (m_testConfig.useExtraDynPCPPipeline)
	{
		std::ostringstream vertDPCP;
		vertDPCP
			<< "#version 450\n"
			<< "\n"
			<< "vec2 positions[3] = vec2[](\n"
			<< "    vec2(-1.0, -1.0),\n"
			<< "    vec2( 3.0, -1.0),\n"
			<< "    vec2(-1.0,  3.0)\n"
			<< ");\n"
			<< "\n"
			<< "void main() {\n"
			<< "    gl_Position = vec4(positions[gl_VertexIndex] + 10.0 + 1.0 * float(gl_VertexIndex), 0.0, 1.0);\n"
			<< "}\n"
			;
		programCollection.glslSources.add("vertDPCP") << glu::VertexSource(vertDPCP.str());

		std::ostringstream fragDPCP;
		fragDPCP
			<< "#version 450\n"
			<< "layout(location=0) out " << vecType << " color;\n"
			<< "void main() {\n"
			<< "    color = " << vecType << "(1.0, 1.0, 1.0, 1.0);\n"
			<< "}\n"
			;
		programCollection.glslSources.add("fragDPCP") << glu::FragmentSource(fragDPCP.str());
	}
}

TestInstance* ExtendedDynamicStateTest::createInstance (Context& context) const
{
	return new ExtendedDynamicStateInstance(context, m_testConfig);
}

ExtendedDynamicStateInstance::ExtendedDynamicStateInstance(Context& context, const TestConfig& testConfig)
	: vkt::TestInstance	(context)
	, m_testConfig		(testConfig)
{
}

struct VertexBufferInfo
{
	VertexBufferInfo ()
		: buffer	()
		, offset	(0ull)
		, dataSize	(0ull)
	{}

	VertexBufferInfo (VertexBufferInfo&& other)
		: buffer	(other.buffer.release())
		, offset	(other.offset)
		, dataSize	(other.dataSize)
	{}

	de::MovePtr<vk::BufferWithMemory>	buffer;
	vk::VkDeviceSize					offset;
	vk::VkDeviceSize					dataSize;
};

void logErrors(tcu::TestLog& log, const std::string& setName, const std::string& setDesc, const tcu::ConstPixelBufferAccess& result, const tcu::ConstPixelBufferAccess& errorMask)
{
	log << tcu::TestLog::ImageSet(setName, setDesc)
		<< tcu::TestLog::Image(setName + "Result", "Result image", result)
		<< tcu::TestLog::Image(setName + "ErrorMask", "Error mask with errors marked in red", errorMask)
		<< tcu::TestLog::EndImageSet;
}

void copyAndFlush(const vk::DeviceInterface& vkd, vk::VkDevice device, vk::BufferWithMemory& buffer, size_t offset, const void* src, size_t size)
{
	auto&	alloc	= buffer.getAllocation();
	auto	dst		= reinterpret_cast<char*>(alloc.getHostPtr());

	deMemcpy(dst + offset, src, size);
	vk::flushAlloc(vkd, device, alloc);
}

// Sets values for dynamic states if needed according to the test configuration.
void setDynamicStates(const TestConfig& testConfig, const vk::DeviceInterface& vkd, vk::VkCommandBuffer cmdBuffer)
{
	if (testConfig.cullModeConfig.dynamicValue)
#ifndef CTS_USES_VULKANSC
		vkd.cmdSetCullMode(cmdBuffer, testConfig.cullModeConfig.dynamicValue.get());
#else
		vkd.cmdSetCullModeEXT(cmdBuffer, testConfig.cullModeConfig.dynamicValue.get());
#endif // CTS_USES_VULKANSC

	if (testConfig.frontFaceConfig.dynamicValue)
#ifndef CTS_USES_VULKANSC
		vkd.cmdSetFrontFace(cmdBuffer, testConfig.frontFaceConfig.dynamicValue.get());
#else
		vkd.cmdSetFrontFaceEXT(cmdBuffer, testConfig.frontFaceConfig.dynamicValue.get());
#endif // CTS_USES_VULKANSC

	if (testConfig.topologyConfig.dynamicValue)
#ifndef CTS_USES_VULKANSC
		vkd.cmdSetPrimitiveTopology(cmdBuffer, testConfig.topologyConfig.dynamicValue.get());
#else
		vkd.cmdSetPrimitiveTopologyEXT(cmdBuffer, testConfig.topologyConfig.dynamicValue.get());
#endif // CTS_USES_VULKANSC

	if (testConfig.viewportConfig.dynamicValue)
	{
		const auto& viewports = testConfig.viewportConfig.dynamicValue.get();
#ifndef CTS_USES_VULKANSC
		vkd.cmdSetViewportWithCount(cmdBuffer, static_cast<deUint32>(viewports.size()), viewports.data());
#else
		vkd.cmdSetViewportWithCountEXT(cmdBuffer, static_cast<deUint32>(viewports.size()), viewports.data());
#endif // CTS_USES_VULKANSC
	}

	if (testConfig.scissorConfig.dynamicValue)
	{
		const auto& scissors = testConfig.scissorConfig.dynamicValue.get();
#ifndef CTS_USES_VULKANSC
		vkd.cmdSetScissorWithCount(cmdBuffer, static_cast<deUint32>(scissors.size()), scissors.data());
#else
		vkd.cmdSetScissorWithCountEXT(cmdBuffer, static_cast<deUint32>(scissors.size()), scissors.data());
#endif // CTS_USES_VULKANSC
	}

	if (testConfig.depthTestEnableConfig.dynamicValue)
#ifndef CTS_USES_VULKANSC
		vkd.cmdSetDepthTestEnable(cmdBuffer, makeVkBool32(testConfig.depthTestEnableConfig.dynamicValue.get()));
#else
		vkd.cmdSetDepthTestEnableEXT(cmdBuffer, makeVkBool32(testConfig.depthTestEnableConfig.dynamicValue.get()));
#endif // CTS_USES_VULKANSC

	if (testConfig.depthWriteEnableConfig.dynamicValue)
#ifndef CTS_USES_VULKANSC
		vkd.cmdSetDepthWriteEnable(cmdBuffer, makeVkBool32(testConfig.depthWriteEnableConfig.dynamicValue.get()));
#else
		vkd.cmdSetDepthWriteEnableEXT(cmdBuffer, makeVkBool32(testConfig.depthWriteEnableConfig.dynamicValue.get()));
#endif // CTS_USES_VULKANSC

	if (testConfig.depthCompareOpConfig.dynamicValue)
#ifndef CTS_USES_VULKANSC
		vkd.cmdSetDepthCompareOp(cmdBuffer, testConfig.depthCompareOpConfig.dynamicValue.get());
#else
		vkd.cmdSetDepthCompareOpEXT(cmdBuffer, testConfig.depthCompareOpConfig.dynamicValue.get());
#endif // CTS_USES_VULKANSC

	if (testConfig.depthBoundsTestEnableConfig.dynamicValue)
#ifndef CTS_USES_VULKANSC
		vkd.cmdSetDepthBoundsTestEnable(cmdBuffer, makeVkBool32(testConfig.depthBoundsTestEnableConfig.dynamicValue.get()));
#else
		vkd.cmdSetDepthBoundsTestEnableEXT(cmdBuffer, makeVkBool32(testConfig.depthBoundsTestEnableConfig.dynamicValue.get()));
#endif // CTS_USES_VULKANSC

	if (testConfig.stencilTestEnableConfig.dynamicValue)
#ifndef CTS_USES_VULKANSC
		vkd.cmdSetStencilTestEnable(cmdBuffer, makeVkBool32(testConfig.stencilTestEnableConfig.dynamicValue.get()));
#else
		vkd.cmdSetStencilTestEnableEXT(cmdBuffer, makeVkBool32(testConfig.stencilTestEnableConfig.dynamicValue.get()));
#endif // CTS_USES_VULKANSC

	if (testConfig.depthBiasEnableConfig.dynamicValue)
#ifndef CTS_USES_VULKANSC
		vkd.cmdSetDepthBiasEnable(cmdBuffer, makeVkBool32(testConfig.depthBiasEnableConfig.dynamicValue.get()));
#else
		vkd.cmdSetDepthBiasEnableEXT(cmdBuffer, makeVkBool32(testConfig.depthBiasEnableConfig.dynamicValue.get()));
#endif // CTS_USES_VULKANSC

	if (testConfig.depthBiasConfig.dynamicValue)
	{
		const auto& bias = testConfig.depthBiasConfig.dynamicValue.get();
		vkd.cmdSetDepthBias(cmdBuffer, bias.constantFactor, bias.clamp, 0.0f);
	}

	if (testConfig.rastDiscardEnableConfig.dynamicValue)
#ifndef CTS_USES_VULKANSC
		vkd.cmdSetRasterizerDiscardEnable(cmdBuffer, makeVkBool32(testConfig.rastDiscardEnableConfig.dynamicValue.get()));
#else
		vkd.cmdSetRasterizerDiscardEnableEXT(cmdBuffer, makeVkBool32(testConfig.rastDiscardEnableConfig.dynamicValue.get()));
#endif // CTS_USES_VULKANSC

	if (testConfig.primRestartEnableConfig.dynamicValue)
#ifndef CTS_USES_VULKANSC
		vkd.cmdSetPrimitiveRestartEnable(cmdBuffer, makeVkBool32(testConfig.primRestartEnableConfig.dynamicValue.get()));
#else
		vkd.cmdSetPrimitiveRestartEnableEXT(cmdBuffer, makeVkBool32(testConfig.primRestartEnableConfig.dynamicValue.get()));
#endif // CTS_USES_VULKANSC

	if (testConfig.logicOpConfig.dynamicValue)
		vkd.cmdSetLogicOpEXT(cmdBuffer, testConfig.logicOpConfig.dynamicValue.get());

	if (testConfig.patchControlPointsConfig.dynamicValue)
		vkd.cmdSetPatchControlPointsEXT(cmdBuffer, testConfig.patchControlPointsConfig.dynamicValue.get());

	if (testConfig.stencilOpConfig.dynamicValue)
	{
		for (const auto& params : testConfig.stencilOpConfig.dynamicValue.get())
#ifndef CTS_USES_VULKANSC
			vkd.cmdSetStencilOp(cmdBuffer, params.faceMask, params.failOp, params.passOp, params.depthFailOp, params.compareOp);
#else
			vkd.cmdSetStencilOpEXT(cmdBuffer, params.faceMask, params.failOp, params.passOp, params.depthFailOp, params.compareOp);
#endif // CTS_USES_VULKANSC
	}

	if (testConfig.vertexGenerator.dynamicValue)
	{
		const auto generator	= testConfig.vertexGenerator.dynamicValue.get();
		const auto bindings		= generator->getBindingDescriptions2(testConfig.strideConfig.staticValue);
		const auto attributes	= generator->getAttributeDescriptions2();

		vkd.cmdSetVertexInputEXT(cmdBuffer,
			static_cast<deUint32>(bindings.size()), de::dataOrNull(bindings),
			static_cast<deUint32>(attributes.size()), de::dataOrNull(attributes));
	}
}

// Bind the appropriate vertex buffers using dynamic strides if the test configuration needs a dynamic stride.
// Return true if the vertex buffer was bound.
bool maybeBindVertexBufferDynStride(const TestConfig& testConfig, const vk::DeviceInterface& vkd, vk::VkCommandBuffer cmdBuffer, size_t meshIdx, const std::vector<VertexBufferInfo>& vertBuffers, const std::vector<VertexBufferInfo>& rvertBuffers)
{
	if (!testConfig.strideConfig.dynamicValue)
		return false;

	DE_ASSERT(!testConfig.useMeshShaders);

	const auto& viewportVec = testConfig.getActiveViewportVec();
	DE_UNREF(viewportVec); // For release builds.

	// When dynamically setting the vertex buffer stride, we cannot bind the vertex buffer in advance for some sequence
	// orderings if we have several viewports or meshes.
	DE_ASSERT((viewportVec.size() == 1u && testConfig.meshParams.size() == 1u)
				|| testConfig.sequenceOrdering == SequenceOrdering::BEFORE_DRAW
				|| testConfig.sequenceOrdering == SequenceOrdering::AFTER_PIPELINES);

	// Split buffers, offsets, sizes and strides into their own vectors for the call.
	std::vector<vk::VkBuffer>		buffers;
	std::vector<vk::VkDeviceSize>	offsets;
	std::vector<vk::VkDeviceSize>	sizes;
	const auto						strides = testConfig.strideConfig.dynamicValue.get();

	const auto& chosenBuffers = (testConfig.meshParams[meshIdx].reversed ? rvertBuffers : vertBuffers);

	buffers.reserve	(chosenBuffers.size());
	offsets.reserve	(chosenBuffers.size());
	sizes.reserve	(chosenBuffers.size());
	DE_ASSERT(chosenBuffers.size() == strides.size());

	for (const auto& vertBuffer : chosenBuffers)
	{
		buffers.push_back	(vertBuffer.buffer->get());
		offsets.push_back	(vertBuffer.offset);
		sizes.push_back		(vertBuffer.dataSize);
	}

#ifndef CTS_USES_VULKANSC
	vkd.cmdBindVertexBuffers2(cmdBuffer, 0u, static_cast<deUint32>(chosenBuffers.size()), buffers.data(), offsets.data(), sizes.data(), strides.data());
#else
	vkd.cmdBindVertexBuffers2EXT(cmdBuffer, 0u, static_cast<deUint32>(chosenBuffers.size()), buffers.data(), offsets.data(), sizes.data(), strides.data());
#endif // CTS_USES_VULKANSC

	return true;
}

// Bind the given vertex buffers with the non-dynamic call. Similar to maybeBindVertexBufferDynStride but simpler.
void bindVertexBuffers (const vk::DeviceInterface& vkd, vk::VkCommandBuffer cmdBuffer, const std::vector<VertexBufferInfo>& vertexBuffers)
{
	std::vector<vk::VkBuffer>		buffers;
	std::vector<vk::VkDeviceSize>	offsets;

	buffers.reserve	(vertexBuffers.size());
	offsets.reserve	(vertexBuffers.size());

	for (const auto& vertBuffer : vertexBuffers)
	{
		buffers.push_back	(vertBuffer.buffer->get());
		offsets.push_back	(vertBuffer.offset);
	}

	vkd.cmdBindVertexBuffers(cmdBuffer, 0u, static_cast<deUint32>(vertexBuffers.size()), buffers.data(), offsets.data());
}

// Create a vector of VertexBufferInfo elements using the given vertex generator and set of vertices.
void prepareVertexBuffers (	std::vector<VertexBufferInfo>&	buffers,
							const vk::DeviceInterface&		vkd,
							vk::VkDevice					device,
							vk::Allocator&					allocator,
							const VertexGenerator*			generator,
							const std::vector<tcu::Vec2>&	vertices,
							deUint32						dataOffset,
							deUint32						trailingSize,
							bool							ssbos)
{
	const deUint32	paddingBytes	= 0xDEADBEEFu;
	const auto		vertexData		= generator->createVertexData(vertices, dataOffset, trailingSize, &paddingBytes, sizeof(paddingBytes));

	for (const auto& bufferBytes : vertexData)
	{
		const auto bufferSize	= static_cast<vk::VkDeviceSize>(de::dataSize(bufferBytes));
		const auto extraSize	= static_cast<vk::VkDeviceSize>(dataOffset + trailingSize);
		DE_ASSERT(bufferSize > extraSize);
		const auto dataSize		= bufferSize - extraSize;

		// Create a full-size buffer but remember the data size and offset for it.
		const auto createInfo = vk::makeBufferCreateInfo(bufferSize, (ssbos ? vk::VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : vk::VK_BUFFER_USAGE_VERTEX_BUFFER_BIT));

		VertexBufferInfo bufferInfo;
		bufferInfo.buffer	= de::MovePtr<vk::BufferWithMemory>(new vk::BufferWithMemory(vkd, device, allocator, createInfo, vk::MemoryRequirement::HostVisible));
		bufferInfo.offset	= static_cast<vk::VkDeviceSize>(dataOffset);
		bufferInfo.dataSize	= dataSize;
		buffers.emplace_back(std::move(bufferInfo));

		// Copy the whole contents to the full buffer.
		copyAndFlush(vkd, device, *buffers.back().buffer, 0, bufferBytes.data(), de::dataSize(bufferBytes));
	}
}

tcu::TestStatus ExtendedDynamicStateInstance::iterate (void)
{
	using ImageWithMemoryVec	= std::vector<std::unique_ptr<vk::ImageWithMemory>>;
	using ImageViewVec			= std::vector<vk::Move<vk::VkImageView>>;
	using FramebufferVec		= std::vector<vk::Move<vk::VkFramebuffer>>;

	const auto&	vki					= m_context.getInstanceInterface();
	const auto&	vkd					= m_context.getDeviceInterface();
	const auto	physicalDevice		= m_context.getPhysicalDevice();
	const auto	device				= m_context.getDevice();
	auto&		allocator			= m_context.getDefaultAllocator();
	const auto	queue				= m_context.getUniversalQueue();
	const auto	queueIndex			= m_context.getUniversalQueueFamilyIndex();
	auto&		log					= m_context.getTestContext().getLog();

	const auto	kReversed			= m_testConfig.isReversed();
	const auto	kNumIterations		= m_testConfig.numIterations();
	const auto	kSequenceOrdering	= m_testConfig.sequenceOrdering;

	const auto						kFramebufferExtent	= vk::makeExtent3D(kFramebufferWidth, kFramebufferHeight, 1u);
	const vk::VkImageUsageFlags		kColorUsage			= (vk::VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | vk::VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	const vk::VkImageUsageFlags		kDSUsage			= (vk::VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | vk::VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	const vk::VkFormatFeatureFlags	kDSFeatures			= (vk::VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | vk::VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
	const auto						colorFormat			= m_testConfig.colorFormat();
	const bool						vertDataAsSSBO		= m_testConfig.useMeshShaders;
	const auto						pipelineBindPoint	= vk::VK_PIPELINE_BIND_POINT_GRAPHICS;

	// Choose depth/stencil format.
	const DepthStencilFormat* dsFormatInfo = nullptr;

	for (const auto& kDepthStencilFormat : kDepthStencilFormats)
	{
		const auto dsProperties = vk::getPhysicalDeviceFormatProperties(vki, physicalDevice, kDepthStencilFormat.imageFormat);
		if ((dsProperties.optimalTilingFeatures & kDSFeatures) == kDSFeatures)
		{
			dsFormatInfo = &kDepthStencilFormat;
			break;
		}
	}

	// Note: Not Supported insted of Fail because the transfer feature is not mandatory.
	if (!dsFormatInfo)
		TCU_THROW(NotSupportedError, "Required depth/stencil image features not supported");
	log << tcu::TestLog::Message << "Chosen depth/stencil format: " << dsFormatInfo->imageFormat << tcu::TestLog::EndMessage;

	// Swap static and dynamic values in the test configuration so the static pipeline ends up with the expected values for cases
	// where we will bind the static pipeline last before drawing.
	if (kReversed)
		m_testConfig.swapValues();

	// Create color and depth/stencil images.
	ImageWithMemoryVec colorImages;
	ImageWithMemoryVec dsImages;

	const vk::VkImageCreateInfo colorImageInfo =
	{
		vk::VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,	//	VkStructureType			sType;
		nullptr,									//	const void*				pNext;
		0u,											//	VkImageCreateFlags		flags;
		vk::VK_IMAGE_TYPE_2D,						//	VkImageType				imageType;
		colorFormat,								//	VkFormat				format;
		kFramebufferExtent,							//	VkExtent3D				extent;
		1u,											//	deUint32				mipLevels;
		1u,											//	deUint32				arrayLayers;
		vk::VK_SAMPLE_COUNT_1_BIT,					//	VkSampleCountFlagBits	samples;
		vk::VK_IMAGE_TILING_OPTIMAL,				//	VkImageTiling			tiling;
		kColorUsage,								//	VkImageUsageFlags		usage;
		vk::VK_SHARING_MODE_EXCLUSIVE,				//	VkSharingMode			sharingMode;
		1u,											//	deUint32				queueFamilyIndexCount;
		&queueIndex,								//	const deUint32*			pQueueFamilyIndices;
		vk::VK_IMAGE_LAYOUT_UNDEFINED,				//	VkImageLayout			initialLayout;
	};
	for (deUint32 i = 0u; i < kNumIterations; ++i)
		colorImages.emplace_back(new vk::ImageWithMemory(vkd, device, allocator, colorImageInfo, vk::MemoryRequirement::Any));

	const vk::VkImageCreateInfo dsImageInfo =
	{
		vk::VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,	//	VkStructureType			sType;
		nullptr,									//	const void*				pNext;
		0u,											//	VkImageCreateFlags		flags;
		vk::VK_IMAGE_TYPE_2D,						//	VkImageType				imageType;
		dsFormatInfo->imageFormat,					//	VkFormat				format;
		kFramebufferExtent,							//	VkExtent3D				extent;
		1u,											//	deUint32				mipLevels;
		1u,											//	deUint32				arrayLayers;
		vk::VK_SAMPLE_COUNT_1_BIT,					//	VkSampleCountFlagBits	samples;
		vk::VK_IMAGE_TILING_OPTIMAL,				//	VkImageTiling			tiling;
		kDSUsage,									//	VkImageUsageFlags		usage;
		vk::VK_SHARING_MODE_EXCLUSIVE,				//	VkSharingMode			sharingMode;
		1u,											//	deUint32				queueFamilyIndexCount;
		&queueIndex,								//	const deUint32*			pQueueFamilyIndices;
		vk::VK_IMAGE_LAYOUT_UNDEFINED,				//	VkImageLayout			initialLayout;
	};
	for (deUint32 i = 0u; i < kNumIterations; ++i)
		dsImages.emplace_back(new vk::ImageWithMemory(vkd, device, allocator, dsImageInfo, vk::MemoryRequirement::Any));

	const auto colorSubresourceRange	= vk::makeImageSubresourceRange(vk::VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u);
	const auto dsSubresourceRange		= vk::makeImageSubresourceRange((vk::VK_IMAGE_ASPECT_DEPTH_BIT | vk::VK_IMAGE_ASPECT_STENCIL_BIT), 0u, 1u, 0u, 1u);

	ImageViewVec colorImageViews;
	ImageViewVec dsImageViews;

	for (const auto& img : colorImages)
		colorImageViews.emplace_back(vk::makeImageView(vkd, device, img->get(), vk::VK_IMAGE_VIEW_TYPE_2D, colorFormat, colorSubresourceRange));

	for (const auto& img : dsImages)
		dsImageViews.emplace_back(vk::makeImageView(vkd, device, img->get(), vk::VK_IMAGE_VIEW_TYPE_2D, dsFormatInfo->imageFormat, dsSubresourceRange));

	// Vertex buffer.
	const auto				topologyClass	= getTopologyClass(m_testConfig.topologyConfig.staticValue);
	std::vector<uint8_t>	indices;
	std::vector<tcu::Vec2>	vertices;

	if (topologyClass == TopologyClass::TRIANGLE)
	{
		// Full-screen triangle strip with 6 vertices.
		//
		// 0        2        4
		//  +-------+-------+
		//  |      XX      X|
		//  |     X X     X |
		//  |    X  X    X  |
		//  |   X   X   X   |
		//  |  X    X  X    |
		//  | X     X X     |
		//  |X      XX      |
		//  +-------+-------+
		// 1        3       5
		vertices.reserve(6u);
		vertices.push_back(tcu::Vec2(-1.0f, -1.0f));
		vertices.push_back(tcu::Vec2(-1.0f,  1.0f));
		vertices.push_back(tcu::Vec2( 0.0f, -1.0f));
		vertices.push_back(tcu::Vec2( 0.0f,  1.0f));
		vertices.push_back(tcu::Vec2( 1.0f, -1.0f));
		vertices.push_back(tcu::Vec2( 1.0f,  1.0f));
	}
	else if (topologyClass == TopologyClass::PATCH)
	{
		DE_ASSERT(m_testConfig.getActivePatchControlPoints() > 1u);

		// 2 triangles making a quad
		vertices.reserve(6u);
		vertices.push_back(tcu::Vec2(-1.0f,  1.0f));
		vertices.push_back(tcu::Vec2( 1.0f,  1.0f));
		vertices.push_back(tcu::Vec2( 1.0f, -1.0f));
		vertices.push_back(tcu::Vec2( 1.0f, -1.0f));
		vertices.push_back(tcu::Vec2(-1.0f, -1.0f));
		vertices.push_back(tcu::Vec2(-1.0f,  1.0f));
	}
	else // TopologyClass::LINE
	{
		// Draw one segmented line per output row of pixels that could be wrongly interpreted as a list of lines that would not cover the whole screen.
		vertices.reserve(kLineVertexCount);
		const float lineHeight = 2.0f / static_cast<float>(kFramebufferHeight);
		for (deUint32 rowIdx = 0; rowIdx < kFramebufferHeight; ++rowIdx)
		{
			// Offset of 0.5 pixels + one line per row from -1 to 1.
			const float yCoord = (lineHeight / 2.0f) + lineHeight * static_cast<float>(rowIdx) - 1.0f;
			vertices.push_back(tcu::Vec2(-1.0f, yCoord));
			vertices.push_back(tcu::Vec2(-0.5f, yCoord));
			vertices.push_back(tcu::Vec2( 0.5f, yCoord));
			vertices.push_back(tcu::Vec2( 1.0f, yCoord));
		}
	}

	if (m_testConfig.singleVertex)
		vertices.resize(1);

	// Reversed vertices order in triangle strip (1, 0, 3, 2, 5, 4)
	std::vector<tcu::Vec2> rvertices;
	if (topologyClass == TopologyClass::TRIANGLE)
	{
		DE_ASSERT(!vertices.empty());
		if (m_testConfig.singleVertex)
			rvertices.push_back(vertices[0]);
		else
		{
			rvertices.reserve(6u);
			rvertices.push_back(vertices[1]);
			rvertices.push_back(vertices[0]);
			rvertices.push_back(vertices[3]);
			rvertices.push_back(vertices[2]);
			rvertices.push_back(vertices[5]);
			rvertices.push_back(vertices[4]);
		}
	}

	if (topologyClass != TopologyClass::TRIANGLE)
	{
		for (const auto& mesh : m_testConfig.meshParams)
		{
			DE_UNREF(mesh); // For release builds.
			DE_ASSERT(!mesh.reversed);
		}
	}

	// Index buffer.
	{
		indices.reserve(kLineVertexCount);
		for (uint32_t i = 0u; i < kLineVertexCount; ++i)
			indices.push_back(static_cast<uint8_t>(i));
	}

	// Buffers with vertex data for the different bindings.
	std::vector<VertexBufferInfo> vertBuffers;
	std::vector<VertexBufferInfo> rvertBuffers;

	{
		const auto dataOffset	= static_cast<deUint32>(m_testConfig.vertexDataOffset);
		const auto trailingSize	= static_cast<deUint32>(m_testConfig.vertexDataExtraBytes);
		const auto generator	= m_testConfig.getActiveVertexGenerator();
		prepareVertexBuffers(vertBuffers, vkd, device, allocator, generator, vertices, dataOffset, trailingSize, vertDataAsSSBO);
		if (topologyClass == TopologyClass::TRIANGLE)
			prepareVertexBuffers(rvertBuffers, vkd, device, allocator, generator, rvertices, dataOffset, trailingSize, vertDataAsSSBO);
	}

	// Index buffer.
	const auto indexDataSize			= static_cast<vk::VkDeviceSize>(de::dataSize(indices));
	const auto indexBufferInfo			= vk::makeBufferCreateInfo(indexDataSize, vk::VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	vk::BufferWithMemory indexBuffer	(vkd, device, allocator, indexBufferInfo, vk::MemoryRequirement::HostVisible);
	copyAndFlush(vkd, device, indexBuffer, 0, indices.data(), static_cast<size_t>(indexDataSize));

	// Push constant stages (matches SSBO stages if used).
	vk::VkShaderStageFlags pushConstantStageFlags = (
		(m_testConfig.useMeshShaders
#ifndef CTS_USES_VULKANSC
		 ? vk::VK_SHADER_STAGE_MESH_BIT_EXT
#else
		 ? 0
#endif // CTS_USES_VULKANSC
		 : vk::VK_SHADER_STAGE_VERTEX_BIT)
		| vk::VK_SHADER_STAGE_FRAGMENT_BIT);

	if (m_testConfig.isMultiViewport() && !m_testConfig.useMeshShaders)
		pushConstantStageFlags |= vk::VK_SHADER_STAGE_GEOMETRY_BIT;

	// Descriptor set layout.
	vk::DescriptorSetLayoutBuilder layoutBuilder;
	if (vertDataAsSSBO)
	{
		for (size_t i = 0; i < vertBuffers.size(); ++i)
		layoutBuilder.addSingleBinding(vk::VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, pushConstantStageFlags);
	}
	const auto descriptorSetLayout = layoutBuilder.build(vkd, device);

	// Descriptor pool and set if needed.
	vk::Move<vk::VkDescriptorPool>	descriptorPool;
	vk::Move<vk::VkDescriptorSet>	descriptorSet;
	vk::Move<vk::VkDescriptorSet>	descriptorSetRev;

	if (vertDataAsSSBO)
	{
		const auto					descType		= vk::VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		vk::DescriptorPoolBuilder	poolBuilder;
		poolBuilder.addType(descType, static_cast<uint32_t>(vertBuffers.size()) * 2u);

		descriptorPool		= poolBuilder.build(vkd, device, vk::VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, 2u);
		descriptorSet		= vk::makeDescriptorSet(vkd, device, descriptorPool.get(), descriptorSetLayout.get());
		descriptorSetRev	= vk::makeDescriptorSet(vkd, device, descriptorPool.get(), descriptorSetLayout.get());

		std::vector<vk::VkDescriptorBufferInfo> descBufferInfos;
		std::vector<vk::VkDescriptorBufferInfo> descBufferInfosRev;
		descBufferInfos.reserve(vertBuffers.size());
		descBufferInfosRev.reserve(rvertBuffers.size());

		vk::DescriptorSetUpdateBuilder updateBuilder;

		DE_ASSERT(vertBuffers.size() == rvertBuffers.size());
		for (size_t i = 0; i < vertBuffers.size(); ++i)
		{
			descBufferInfos.push_back(vk::makeDescriptorBufferInfo(vertBuffers[i].buffer->get(), vertBuffers[i].offset, vertBuffers[i].dataSize));
			descBufferInfosRev.push_back(vk::makeDescriptorBufferInfo(rvertBuffers[i].buffer->get(), rvertBuffers[i].offset, rvertBuffers[i].dataSize));

			const auto binding = vk::DescriptorSetUpdateBuilder::Location::binding(static_cast<uint32_t>(i));

			updateBuilder.writeSingle(descriptorSet.get(), binding, descType, &descBufferInfos.back());
			updateBuilder.writeSingle(descriptorSetRev.get(), binding, descType, &descBufferInfosRev.back());
		}

		updateBuilder.update(vkd, device);
	}

	// Descriptor set if needed.
	if (vertDataAsSSBO)
	{
	}

	// Pipeline layout.
	const vk::VkPushConstantRange pushConstantRange =
	{
		pushConstantStageFlags,							//	VkShaderStageFlags	stageFlags;
		0u,												//	deUint32			offset;
		static_cast<deUint32>(sizeof(PushConstants)),	//	deUint32			size;
	};

	const vk::VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo =
	{
		vk::VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,	//	VkStructureType					sType;
		nullptr,											//	const void*						pNext;
		0u,													//	VkPipelineLayoutCreateFlags		flags;
		1u,													//	deUint32						setLayoutCount;
		&descriptorSetLayout.get(),							//	const VkDescriptorSetLayout*	pSetLayouts;
		1u,													//	deUint32						pushConstantRangeCount;
		&pushConstantRange,									//	const VkPushConstantRange*		pPushConstantRanges;
	};
	const auto pipelineLayout = vk::createPipelineLayout(vkd, device, &pipelineLayoutCreateInfo);

	// Render pass with single subpass.
	const vk::VkAttachmentReference colorAttachmentReference =
	{
		0u,												//	deUint32		attachment;
		vk::VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,	//	VkImageLayout	layout;
	};

	const vk::VkAttachmentReference dsAttachmentReference =
	{
		1u,														//	deUint32		attachment;
		vk::VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,	//	VkImageLayout	layout;
	};

	const vk::VkSubpassDescription subpassDescription =
	{
		0u,										//	VkSubpassDescriptionFlags		flags;
		pipelineBindPoint,						//	VkPipelineBindPoint				pipelineBindPoint;
		0u,										//	deUint32						inputAttachmentCount;
		nullptr,								//	const VkAttachmentReference*	pInputAttachments;
		1u,										//	deUint32						colorAttachmentCount;
		&colorAttachmentReference,				//	const VkAttachmentReference*	pColorAttachments;
		nullptr,								//	const VkAttachmentReference*	pResolveAttachments;
		&dsAttachmentReference,					//	const VkAttachmentReference*	pDepthStencilAttachment;
		0u,										//	deUint32						preserveAttachmentCount;
		nullptr,								//	const deUint32*					pPreserveAttachments;
	};

	std::vector<vk::VkAttachmentDescription> attachmentDescriptions;

	attachmentDescriptions.push_back(vk::VkAttachmentDescription
	{
		0u,												//	VkAttachmentDescriptionFlags	flags;
		colorFormat,									//	VkFormat						format;
		vk::VK_SAMPLE_COUNT_1_BIT,						//	VkSampleCountFlagBits			samples;
		vk::VK_ATTACHMENT_LOAD_OP_CLEAR,				//	VkAttachmentLoadOp				loadOp;
		vk::VK_ATTACHMENT_STORE_OP_STORE,				//	VkAttachmentStoreOp				storeOp;
		vk::VK_ATTACHMENT_LOAD_OP_DONT_CARE,			//	VkAttachmentLoadOp				stencilLoadOp;
		vk::VK_ATTACHMENT_STORE_OP_DONT_CARE,			//	VkAttachmentStoreOp				stencilStoreOp;
		vk::VK_IMAGE_LAYOUT_UNDEFINED,					//	VkImageLayout					initialLayout;
		vk::VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,	//	VkImageLayout					finalLayout;
	});

	attachmentDescriptions.push_back(vk::VkAttachmentDescription
	{
		0u,														//	VkAttachmentDescriptionFlags	flags;
		dsFormatInfo->imageFormat,								//	VkFormat						format;
		vk::VK_SAMPLE_COUNT_1_BIT,								//	VkSampleCountFlagBits			samples;
		vk::VK_ATTACHMENT_LOAD_OP_CLEAR,						//	VkAttachmentLoadOp				loadOp;
		vk::VK_ATTACHMENT_STORE_OP_STORE,						//	VkAttachmentStoreOp				storeOp;
		vk::VK_ATTACHMENT_LOAD_OP_CLEAR,						//	VkAttachmentLoadOp				stencilLoadOp;
		vk::VK_ATTACHMENT_STORE_OP_STORE,						//	VkAttachmentStoreOp				stencilStoreOp;
		vk::VK_IMAGE_LAYOUT_UNDEFINED,							//	VkImageLayout					initialLayout;
		vk::VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,	//	VkImageLayout					finalLayout;
	});

	const vk::VkRenderPassCreateInfo renderPassCreateInfo =
	{
		vk::VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,			//	VkStructureType					sType;
		nullptr,												//	const void*						pNext;
		0u,														//	VkRenderPassCreateFlags			flags;
		static_cast<deUint32>(attachmentDescriptions.size()),	//	deUint32						attachmentCount;
		attachmentDescriptions.data(),							//	const VkAttachmentDescription*	pAttachments;
		1u,														//	deUint32						subpassCount;
		&subpassDescription,									//	const VkSubpassDescription*		pSubpasses;
		0u,														//	deUint32						dependencyCount;
		nullptr,												//	const VkSubpassDependency*		pDependencies;
	};
	const auto renderPass = vk::createRenderPass(vkd, device, &renderPassCreateInfo);

	// Framebuffers.
	FramebufferVec framebuffers;

	DE_ASSERT(colorImageViews.size() == dsImageViews.size());
	for (size_t imgIdx = 0; imgIdx < colorImageViews.size(); ++imgIdx)
	{
		std::vector<vk::VkImageView> attachments;
		attachments.push_back(colorImageViews[imgIdx].get());
		attachments.push_back(dsImageViews[imgIdx].get());

		const vk::VkFramebufferCreateInfo framebufferCreateInfo =
		{
			vk::VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,	//	VkStructureType				sType;
			nullptr,										//	const void*					pNext;
			0u,												//	VkFramebufferCreateFlags	flags;
			renderPass.get(),								//	VkRenderPass				renderPass;
			static_cast<deUint32>(attachments.size()),		//	deUint32					attachmentCount;
			attachments.data(),								//	const VkImageView*			pAttachments;
			kFramebufferWidth,								//	deUint32					width;
			kFramebufferHeight,								//	deUint32					height;
			1u,												//	deUint32					layers;
		};

		framebuffers.emplace_back(vk::createFramebuffer(vkd, device, &framebufferCreateInfo));
	}

	// Shader modules.
	const auto&	binaries			= m_context.getBinaryCollection();
	const auto	dynamicVertModule	= vk::createShaderModule(vkd, device, binaries.get("dynamicVert"));
	const auto	staticVertModule	= vk::createShaderModule(vkd, device, binaries.get("staticVert"));
	const auto	fragModule			= vk::createShaderModule(vkd, device, binaries.get("frag"));
	const auto	geomModule			= (m_testConfig.needsGeometryShader() ? vk::createShaderModule(vkd, device, binaries.get("geom")) : vk::Move<vk::VkShaderModule>());
	const auto	tescModule			= (m_testConfig.needsTessellation() ? vk::createShaderModule(vkd, device, binaries.get("tesc")) : vk::Move<vk::VkShaderModule>());
	const auto	teseModule			= (m_testConfig.needsTessellation() ? vk::createShaderModule(vkd, device, binaries.get("tese")) : vk::Move<vk::VkShaderModule>());
	const auto	meshModule			= (m_testConfig.useMeshShaders ? vk::createShaderModule(vkd, device, binaries.get("mesh")) : vk::Move<vk::VkShaderModule>());
	const auto	meshNoOutModule		= (m_testConfig.bindUnusedMeshShadingPipeline ? vk::createShaderModule(vkd, device, binaries.get("meshNoOut")) : vk::Move<vk::VkShaderModule>());

	vk::Move<vk::VkShaderModule>	vertDPCPModule;
	vk::Move<vk::VkShaderModule>	fragDPCPModule;

	// Input state.
	const auto vertexBindings	= m_testConfig.vertexGenerator.staticValue->getBindingDescriptions(m_testConfig.strideConfig.staticValue);
	const auto vertexAttributes	= m_testConfig.vertexGenerator.staticValue->getAttributeDescriptions();

	const vk::VkPipelineVertexInputStateCreateInfo vertexInputStateCreateInfo =
	{
		vk::VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,	//	VkStructureType								sType;
		nullptr,														//	const void*									pNext;
		0u,																//	VkPipelineVertexInputStateCreateFlags		flags;
		static_cast<deUint32>(vertexBindings.size()),					//	deUint32									vertexBindingDescriptionCount;
		vertexBindings.data(),											//	const VkVertexInputBindingDescription*		pVertexBindingDescriptions;
		static_cast<deUint32>(vertexAttributes.size()),					//	deUint32									vertexAttributeDescriptionCount;
		vertexAttributes.data(),										//	const VkVertexInputAttributeDescription*	pVertexAttributeDescriptions;
	};

	// Input assembly.
	const vk::VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCreateInfo =
	{
		vk::VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,	//	VkStructureType							sType;
		nullptr,															//	const void*								pNext;
		0u,																	//	VkPipelineInputAssemblyStateCreateFlags	flags;
		m_testConfig.topologyConfig.staticValue,							//	VkPrimitiveTopology						topology;
		makeVkBool32(m_testConfig.primRestartEnableConfig.staticValue),		//	VkBool32								primitiveRestartEnable;
	};

	// Viewport state.
	if (m_testConfig.viewportConfig.dynamicValue)
		DE_ASSERT(m_testConfig.viewportConfig.dynamicValue.get().size() > 0u);
	else
		DE_ASSERT(m_testConfig.viewportConfig.staticValue.size() > 0u);

	if (m_testConfig.scissorConfig.dynamicValue)
		DE_ASSERT(m_testConfig.scissorConfig.dynamicValue.get().size() > 0u);
	else
		DE_ASSERT(m_testConfig.scissorConfig.staticValue.size() > 0u);

	// Rasterization state.
	vk::VkPipelineRasterizationStateCreateInfo rasterizationStateCreateInfo
	{
		vk::VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,	//	VkStructureType							sType;
		nullptr,														//	const void*								pNext;
		0u,																//	VkPipelineRasterizationStateCreateFlags	flags;
		VK_FALSE,														//	VkBool32								depthClampEnable;
		makeVkBool32(m_testConfig.rastDiscardEnableConfig.staticValue),	//	VkBool32								rasterizerDiscardEnable;
		vk::VK_POLYGON_MODE_FILL,										//	VkPolygonMode							polygonMode;
		m_testConfig.cullModeConfig.staticValue,						//	VkCullModeFlags							cullMode;
		m_testConfig.frontFaceConfig.staticValue,						//	VkFrontFace								frontFace;
		makeVkBool32(m_testConfig.depthBiasEnableConfig.staticValue),	//	VkBool32								depthBiasEnable;
		m_testConfig.depthBiasConfig.staticValue.constantFactor,		//	float									depthBiasConstantFactor;
		m_testConfig.depthBiasConfig.staticValue.clamp,					//	float									depthBiasClamp;
		0.0f,															//	float									depthBiasSlopeFactor;
		1.0f,															//	float									lineWidth;
	};

	// Multisample state.
	const vk::VkPipelineMultisampleStateCreateInfo multisampleStateCreateInfo
	{
		vk::VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,	//	VkStructureType							sType;
		nullptr,														//	const void*								pNext;
		0u,																//	VkPipelineMultisampleStateCreateFlags	flags;
		vk::VK_SAMPLE_COUNT_1_BIT,										//	VkSampleCountFlagBits					rasterizationSamples;
		VK_FALSE,														//	VkBool32								sampleShadingEnable;
		0.0f,															//	float									minSampleShading;
		nullptr,														//	const VkSampleMask*						pSampleMask;
		VK_FALSE,														//	VkBool32								alphaToCoverageEnable;
		VK_FALSE,														//	VkBool32								alphaToOneEnable;
	};

	// Depth/stencil state.
	vk::VkStencilOpState	staticFrontStencil;
	vk::VkStencilOpState	staticBackStencil;
	bool					staticFrontStencilSet	= false;
	bool					staticBackStencilSet	= false;

	// Common setup for the front and back operations.
	staticFrontStencil.compareMask	= 0xFFu;
	staticFrontStencil.writeMask	= 0xFFu;
	staticFrontStencil.reference	= m_testConfig.referenceStencil;
	staticBackStencil				= staticFrontStencil;

	for (const auto& op : m_testConfig.stencilOpConfig.staticValue)
	{
		if ((op.faceMask & vk::VK_STENCIL_FACE_FRONT_BIT) != 0u)
		{
			copy(staticFrontStencil, op);
			staticFrontStencilSet = true;
		}
		if ((op.faceMask & vk::VK_STENCIL_FACE_BACK_BIT) != 0u)
		{
			copy(staticBackStencil, op);
			staticBackStencilSet = true;
		}
	}

	// Default values for the static part.
	if (!staticFrontStencilSet)
		copy(staticFrontStencil, kDefaultStencilOpParams);
	if (!staticBackStencilSet)
		copy(staticBackStencil, kDefaultStencilOpParams);

	const vk::VkPipelineDepthStencilStateCreateInfo depthStencilStateCreateInfo =
	{
		vk::VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,		//	VkStructureType							sType;
		nullptr,															//	const void*								pNext;
		0u,																	//	VkPipelineDepthStencilStateCreateFlags	flags;
		makeVkBool32(m_testConfig.depthTestEnableConfig.staticValue),		//	VkBool32								depthTestEnable;
		makeVkBool32(m_testConfig.depthWriteEnableConfig.staticValue),		//	VkBool32								depthWriteEnable;
		m_testConfig.depthCompareOpConfig.staticValue,						//	VkCompareOp								depthCompareOp;
		makeVkBool32(m_testConfig.depthBoundsTestEnableConfig.staticValue),	//	VkBool32								depthBoundsTestEnable;
		makeVkBool32(m_testConfig.stencilTestEnableConfig.staticValue),		//	VkBool32								stencilTestEnable;
		staticFrontStencil,													//	VkStencilOpState						front;
		staticBackStencil,													//	VkStencilOpState						back;
		m_testConfig.minDepthBounds,										//	float									minDepthBounds;
		m_testConfig.maxDepthBounds,										//	float									maxDepthBounds;
	};

	// Dynamic state. Here we will set all states which have a dynamic value.
	const auto dynamicStates = m_testConfig.getDynamicStates();

	const vk::VkPipelineDynamicStateCreateInfo dynamicStateCreateInfo =
	{
		vk::VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,	//	VkStructureType						sType;
		nullptr,													//	const void*							pNext;
		0u,															//	VkPipelineDynamicStateCreateFlags	flags;
		static_cast<deUint32>(dynamicStates.size()),				//	deUint32							dynamicStateCount;
		de::dataOrNull(dynamicStates),								//	const VkDynamicState*				pDynamicStates;
	};

	const vk::VkPipelineColorBlendAttachmentState colorBlendAttachmentState =
	{
		VK_FALSE,						// VkBool32                 blendEnable
		vk::VK_BLEND_FACTOR_ZERO,		// VkBlendFactor            srcColorBlendFactor
		vk::VK_BLEND_FACTOR_ZERO,		// VkBlendFactor            dstColorBlendFactor
		vk::VK_BLEND_OP_ADD,			// VkBlendOp                colorBlendOp
		vk::VK_BLEND_FACTOR_ZERO,		// VkBlendFactor            srcAlphaBlendFactor
		vk::VK_BLEND_FACTOR_ZERO,		// VkBlendFactor            dstAlphaBlendFactor
		vk::VK_BLEND_OP_ADD,			// VkBlendOp                alphaBlendOp
		vk::VK_COLOR_COMPONENT_R_BIT	// VkColorComponentFlags    colorWriteMask
		| vk::VK_COLOR_COMPONENT_G_BIT
		| vk::VK_COLOR_COMPONENT_B_BIT
		| vk::VK_COLOR_COMPONENT_A_BIT
	};

	const vk::VkPipelineColorBlendStateCreateInfo colorBlendStateCreateInfo =
	{
		vk::VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,	// VkStructureType                               sType
		nullptr,														// const void*                                   pNext
		0u,																// VkPipelineColorBlendStateCreateFlags          flags
		makeVkBool32(m_testConfig.testLogicOp()),						// VkBool32                                      logicOpEnable
		m_testConfig.logicOpConfig.staticValue,							// VkLogicOp                                     logicOp
		1u,																// deUint32                                      attachmentCount
		&colorBlendAttachmentState,										// const VkPipelineColorBlendAttachmentState*    pAttachments
		{ 0.0f, 0.0f, 0.0f, 0.0f }										// float                                         blendConstants[4]
	};

	vk::GraphicsPipelineWrapper	staticPipeline		(vkd, device, m_testConfig.pipelineConstructionType);
	const bool					bindStaticFirst		= (kSequenceOrdering == SequenceOrdering::BETWEEN_PIPELINES	||
													   kSequenceOrdering == SequenceOrdering::AFTER_PIPELINES	||
													   kSequenceOrdering == SequenceOrdering::TWO_DRAWS_DYNAMIC);
	const bool					useStaticPipeline	= (bindStaticFirst || kReversed);

	// Create extra dynamic patch control points pipeline if needed.
	vk::Move<vk::VkPipeline> extraDynPCPPipeline;

	if (m_testConfig.useExtraDynPCPPipeline)
	{
		vertDPCPModule = vk::createShaderModule(vkd, device, m_context.getBinaryCollection().get("vertDPCP"));
		fragDPCPModule = vk::createShaderModule(vkd, device, m_context.getBinaryCollection().get("fragDPCP"));

		const vk::VkPipelineVertexInputStateCreateInfo	extraDPCPInputState		= vk::initVulkanStructure();
		const vk::VkDynamicState						extraDynamicState		= vk::VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT;
		const vk::VkPipelineDynamicStateCreateInfo		extraDynamicStateInfo	=
		{
			vk::VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,	//	VkStructureType						sType;
			nullptr,													//	const void*							pNext;
			0u,															//	VkPipelineDynamicStateCreateFlags	flags;
			1u,															//	uint32_t							dynamicStateCount;
			&extraDynamicState,												//	const VkDynamicState*				pDynamicStates;
		};

		const auto extraPipelineLayout = vk::makePipelineLayout(vkd, device);

		const auto viewports	= m_testConfig.viewportConfig.staticValue;
		const auto scissors		= m_testConfig.scissorConfig.staticValue;

		extraDynPCPPipeline = vk::makeGraphicsPipeline(
			vkd, device, *extraPipelineLayout,
			vertDPCPModule.get(), DE_NULL, DE_NULL, DE_NULL, fragDPCPModule.get(),
			renderPass.get(), viewports, scissors, vk::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0u, 0u,
			&extraDPCPInputState, nullptr, nullptr, nullptr, nullptr, &extraDynamicStateInfo);
	}

	// Create static pipeline when needed.
	if (useStaticPipeline)
	{
		auto viewports	= m_testConfig.viewportConfig.staticValue;
		auto scissors	= m_testConfig.scissorConfig.staticValue;

		// The viewport and scissor counts must match in the static part, which will be used by the static pipeline.
		const auto minStaticCount = static_cast<deUint32>(std::min(m_testConfig.viewportConfig.staticValue.size(), m_testConfig.scissorConfig.staticValue.size()));
		viewports.resize(minStaticCount);
		scissors.resize(minStaticCount);

		staticPipeline.setDefaultPatchControlPoints(m_testConfig.patchControlPointsConfig.staticValue);

#ifndef CTS_USES_VULKANSC
		if (m_testConfig.useMeshShaders)
		{
			staticPipeline.setupPreRasterizationMeshShaderState(
												viewports,
												scissors,
												*pipelineLayout,
												*renderPass,
												0u,
												DE_NULL,
												*meshModule,
												&rasterizationStateCreateInfo);
		}
		else
#endif // CTS_USES_VULKANSC
		{
			staticPipeline.setupVertexInputState(&vertexInputStateCreateInfo, &inputAssemblyStateCreateInfo)
						  .setupPreRasterizationShaderState(
												viewports,
												scissors,
												*pipelineLayout,
												*renderPass,
												0u,
												*staticVertModule,
												&rasterizationStateCreateInfo,
												*tescModule,
												*teseModule,
												*geomModule);
		}

		staticPipeline.setupFragmentShaderState(*pipelineLayout, *renderPass, 0u, *fragModule, &depthStencilStateCreateInfo, &multisampleStateCreateInfo)
					  .setupFragmentOutputState(*renderPass, 0u, &colorBlendStateCreateInfo, &multisampleStateCreateInfo)
					  .setMonolithicPipelineLayout(*pipelineLayout)
					  .buildPipeline();
	}

	// Create dynamic pipeline.
	vk::GraphicsPipelineWrapper graphicsPipeline(vkd, device, m_testConfig.pipelineConstructionType);
	{
		auto viewports	= m_testConfig.viewportConfig.staticValue;
		auto scissors	= m_testConfig.scissorConfig.staticValue;

		const auto finalDynamicViewportCount = (m_testConfig.viewportConfig.dynamicValue
			? m_testConfig.viewportConfig.dynamicValue.get().size()
			: m_testConfig.viewportConfig.staticValue.size());

		const auto finalDynamicScissorCount = (m_testConfig.scissorConfig.dynamicValue
			? m_testConfig.scissorConfig.dynamicValue.get().size()
			: m_testConfig.scissorConfig.staticValue.size());

		const auto minDynamicCount = static_cast<deUint32>(std::min(finalDynamicScissorCount, finalDynamicViewportCount));

		// The viewport and scissor counts must be zero when a dynamic value will be provided, as per the spec.
		if (m_testConfig.viewportConfig.dynamicValue)
		{
			graphicsPipeline.setDefaultViewportsCount();
			viewports = std::vector<vk::VkViewport>();
		}
		else
			viewports.resize(minDynamicCount);

		if (m_testConfig.scissorConfig.dynamicValue)
		{
			graphicsPipeline.setDefaultScissorsCount();
			scissors = std::vector<vk::VkRect2D>();
		}
		else
			scissors.resize(minDynamicCount);

		graphicsPipeline.setDynamicState(&dynamicStateCreateInfo)
						.setDefaultPatchControlPoints(m_testConfig.patchControlPointsConfig.staticValue);

#ifndef CTS_USES_VULKANSC
		if (m_testConfig.useMeshShaders)
		{
			graphicsPipeline.setupPreRasterizationMeshShaderState(
												viewports,
												scissors,
												*pipelineLayout,
												*renderPass,
												0u,
												DE_NULL,
												*meshModule,
												&rasterizationStateCreateInfo);
		}
		else
#endif // CTS_USES_VULKANSC
		{
			graphicsPipeline.setupVertexInputState(&vertexInputStateCreateInfo, &inputAssemblyStateCreateInfo)
							.setupPreRasterizationShaderState(
												viewports,
												scissors,
												*pipelineLayout,
												*renderPass,
												0u,
												*dynamicVertModule,
												&rasterizationStateCreateInfo,
												*tescModule,
												*teseModule,
												*geomModule);
		}

		graphicsPipeline.setupFragmentShaderState(*pipelineLayout, *renderPass, 0u, *fragModule, &depthStencilStateCreateInfo, &multisampleStateCreateInfo)
						.setupFragmentOutputState(*renderPass, 0u, &colorBlendStateCreateInfo, &multisampleStateCreateInfo)
						.setMonolithicPipelineLayout(*pipelineLayout)
						.buildPipeline();
	}

	vk::GraphicsPipelineWrapper meshNoOutPipeline(vkd, device, m_testConfig.pipelineConstructionType);

#ifndef CTS_USES_VULKANSC
	if (m_testConfig.bindUnusedMeshShadingPipeline)
	{
		// Remove dynamic states which are not compatible with mesh shading pipelines.
		std::vector<vk::VkDynamicState> meshNoOutDynamicStates;
		std::copy_if(begin(dynamicStates), end(dynamicStates), std::back_inserter(meshNoOutDynamicStates), isMeshShadingPipelineCompatible);

		const vk::VkPipelineDynamicStateCreateInfo meshNoOutDynamicStateInfo =
		{
			vk::VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,	//	VkStructureType						sType;
			nullptr,													//	const void*							pNext;
			0u,															//	VkPipelineDynamicStateCreateFlags	flags;
			static_cast<uint32_t>(meshNoOutDynamicStates.size()),		//	uint32_t							dynamicStateCount;
			de::dataOrNull(meshNoOutDynamicStates),						//	const VkDynamicState*				pDynamicStates;
		};

		// Provide a viewport state similar to the static pipeline.
		auto viewports	= m_testConfig.viewportConfig.staticValue;
		auto scissors	= m_testConfig.scissorConfig.staticValue;

		const auto minStaticCount = static_cast<deUint32>(std::min(m_testConfig.viewportConfig.staticValue.size(), m_testConfig.scissorConfig.staticValue.size()));
		viewports.resize(minStaticCount);
		scissors.resize(minStaticCount);

		meshNoOutPipeline.setDynamicState(&meshNoOutDynamicStateInfo)
						 .setupPreRasterizationMeshShaderState(
											viewports,
											scissors,
											*pipelineLayout,
											*renderPass,
											0u,
											DE_NULL,
											*meshNoOutModule,
											&rasterizationStateCreateInfo)
						 .setupFragmentShaderState(*pipelineLayout, *renderPass, 0u, DE_NULL, &depthStencilStateCreateInfo, &multisampleStateCreateInfo)
						 .setupFragmentOutputState(*renderPass, 0u, &colorBlendStateCreateInfo, &multisampleStateCreateInfo)
						 .setMonolithicPipelineLayout(*pipelineLayout)
						 .buildPipeline();
	}
#endif // CTS_USES_VULKANSC

	// Command buffer.
	const auto cmdPool		= vk::makeCommandPool(vkd, device, queueIndex);
	const auto cmdBufferPtr	= vk::allocateCommandBuffer(vkd , device, cmdPool.get(), vk::VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	const auto cmdBuffer	= cmdBufferPtr.get();

	// Clear values, clear to green for dynamic logicOp
	std::vector<vk::VkClearValue> clearValues;
	clearValues.push_back(m_testConfig.clearColorValue);
	clearValues.push_back(vk::makeClearValueDepthStencil(m_testConfig.clearDepthValue, m_testConfig.clearStencilValue));

	// Record command buffer.
	vk::beginCommandBuffer(vkd, cmdBuffer);

	for (deUint32 iteration = 0u; iteration < kNumIterations; ++iteration)
	{
		// Track in-advance vertex buffer binding.
		bool boundInAdvance = false;

		// Maybe set extended dynamic state here.
		if (kSequenceOrdering == SequenceOrdering::CMD_BUFFER_START)
		{
			setDynamicStates(m_testConfig, vkd, cmdBuffer);
			boundInAdvance = maybeBindVertexBufferDynStride(m_testConfig, vkd, cmdBuffer, 0u, vertBuffers, rvertBuffers);
		}

		// Begin render pass.
		vk::beginRenderPass(vkd, cmdBuffer, renderPass.get(), framebuffers[iteration].get(), vk::makeRect2D(kFramebufferWidth, kFramebufferHeight), static_cast<deUint32>(clearValues.size()), clearValues.data());

			// Bind a static pipeline first if needed.
			if (bindStaticFirst && iteration == 0u)
				vkd.cmdBindPipeline(cmdBuffer, pipelineBindPoint, staticPipeline.getPipeline());

			// Maybe set extended dynamic state here.
			if (kSequenceOrdering == SequenceOrdering::BETWEEN_PIPELINES)
			{
				setDynamicStates(m_testConfig, vkd, cmdBuffer);
				boundInAdvance = maybeBindVertexBufferDynStride(m_testConfig, vkd, cmdBuffer, 0u, vertBuffers, rvertBuffers);
			}

			// Bind dynamic pipeline.
			if ((kSequenceOrdering != SequenceOrdering::TWO_DRAWS_DYNAMIC &&
				 kSequenceOrdering != SequenceOrdering::TWO_DRAWS_STATIC) ||
				(kSequenceOrdering == SequenceOrdering::TWO_DRAWS_DYNAMIC && iteration > 0u) ||
				(kSequenceOrdering == SequenceOrdering::TWO_DRAWS_STATIC && iteration == 0u))
			{
				if (m_testConfig.bindUnusedMeshShadingPipeline)
				{
					DE_ASSERT(kSequenceOrdering == SequenceOrdering::CMD_BUFFER_START);
					vkd.cmdBindPipeline(cmdBuffer, pipelineBindPoint, meshNoOutPipeline.getPipeline());
				}

				if (m_testConfig.useExtraDynPCPPipeline)
				{
					vkd.cmdBindPipeline(cmdBuffer, pipelineBindPoint, extraDynPCPPipeline.get());

					// In these two sequence orderings, the right dynamic state value will have been set before and we would be
					// setting it to a wrong value here, resulting in test failures. We keep the right value instead.
					if (kSequenceOrdering != SequenceOrdering::CMD_BUFFER_START && kSequenceOrdering != SequenceOrdering::BETWEEN_PIPELINES)
						vkd.cmdSetPatchControlPointsEXT(cmdBuffer, m_testConfig.patchControlPointsConfig.staticValue);

					vkd.cmdDraw(cmdBuffer, 3u, 1u, 0u, 0u);
				}

				vkd.cmdBindPipeline(cmdBuffer, pipelineBindPoint, graphicsPipeline.getPipeline());
			}

			if (kSequenceOrdering == SequenceOrdering::BEFORE_GOOD_STATIC ||
				(kSequenceOrdering == SequenceOrdering::TWO_DRAWS_DYNAMIC && iteration > 0u) ||
				(kSequenceOrdering == SequenceOrdering::TWO_DRAWS_STATIC && iteration == 0u))
			{
				setDynamicStates(m_testConfig, vkd, cmdBuffer);
				boundInAdvance = maybeBindVertexBufferDynStride(m_testConfig, vkd, cmdBuffer, 0u, vertBuffers, rvertBuffers);
			}

			// Bind a static pipeline last if needed.
			if (kSequenceOrdering == SequenceOrdering::BEFORE_GOOD_STATIC ||
				(kSequenceOrdering == SequenceOrdering::TWO_DRAWS_STATIC && iteration > 0u))
			{
				vkd.cmdBindPipeline(cmdBuffer, pipelineBindPoint, staticPipeline.getPipeline());
			}

			const auto& viewportVec = m_testConfig.getActiveViewportVec();
			for (size_t viewportIdx = 0u; viewportIdx < viewportVec.size(); ++viewportIdx)
			{
				for (size_t meshIdx = 0u; meshIdx < m_testConfig.meshParams.size(); ++meshIdx)
				{
					// Push constants.
					PushConstants pushConstants =
					{
						m_testConfig.meshParams[meshIdx].color,			//	tcu::Vec4	triangleColor;
						m_testConfig.meshParams[meshIdx].depth,			//	float		meshDepth;
						static_cast<deInt32>(viewportIdx),				//	deInt32		viewPortIndex;
						m_testConfig.meshParams[meshIdx].scaleX,		//	float		scaleX;
						m_testConfig.meshParams[meshIdx].scaleY,		//	float		scaleY;
						m_testConfig.meshParams[meshIdx].offsetX,		//	float		offsetX;
						m_testConfig.meshParams[meshIdx].offsetY,		//	float		offsetY;
						m_testConfig.meshParams[meshIdx].stripScale,	//	float		stripScale;
					};
					vkd.cmdPushConstants(cmdBuffer, pipelineLayout.get(), pushConstantStageFlags, 0u, static_cast<deUint32>(sizeof(pushConstants)), &pushConstants);

					// Track vertex bounding state for this mesh.
					bool boundBeforeDraw = false;

					// Maybe set extended dynamic state here.
					if (kSequenceOrdering == SequenceOrdering::BEFORE_DRAW || kSequenceOrdering == SequenceOrdering::AFTER_PIPELINES)
					{
						setDynamicStates(m_testConfig, vkd, cmdBuffer);
						boundBeforeDraw = maybeBindVertexBufferDynStride(m_testConfig, vkd, cmdBuffer, meshIdx, vertBuffers, rvertBuffers);
					}

					// Bind vertex buffer with static stride if needed and draw.
					if (!(boundInAdvance || boundBeforeDraw) && !m_testConfig.useMeshShaders)
					{
						bindVertexBuffers(vkd, cmdBuffer, (m_testConfig.meshParams[meshIdx].reversed ? rvertBuffers : vertBuffers));
						if (m_testConfig.needsIndexBuffer())
						{
							const auto indexType = vk::VK_INDEX_TYPE_UINT8_EXT;
							vkd.cmdBindIndexBuffer(cmdBuffer, indexBuffer.get(), 0, indexType);
						}
					}

					if (vertDataAsSSBO)
					{
						const auto boundSet = (m_testConfig.meshParams[meshIdx].reversed ? descriptorSetRev.get() : descriptorSet.get());
						vkd.cmdBindDescriptorSets(cmdBuffer, pipelineBindPoint, pipelineLayout.get(), 0u, 1u, &boundSet, 0u, nullptr);
					}

					// Draw mesh.
					if (m_testConfig.needsIndexBuffer())
					{
						const auto numIndices = static_cast<uint32_t>(indices.size());
						vkd.cmdDrawIndexed(cmdBuffer, numIndices, 1u, 0u, 0u, 0u);
					}
#ifndef CTS_USES_VULKANSC
					else if (m_testConfig.useMeshShaders)
					{
						// Make sure drawing this way makes sense.
						DE_ASSERT(vertices.size() > 2u);
						DE_ASSERT(!m_testConfig.topologyConfig.dynamicValue);
						DE_ASSERT(m_testConfig.topologyConfig.staticValue == vk::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);

						const auto numPrimitives = static_cast<uint32_t>(vertices.size()) - 2u;
						vkd.cmdDrawMeshTasksEXT(cmdBuffer, numPrimitives, 1u, 1u);
					}
#endif // CTS_USES_VULKANSC
					else
					{
						deUint32 vertex_count = static_cast<deUint32>(vertices.size());
						if (m_testConfig.singleVertex)
							vertex_count = m_testConfig.singleVertexDrawCount;
						vkd.cmdDraw(cmdBuffer, vertex_count, 1u, 0u, 0u);
					}
				}
			}

		vk::endRenderPass(vkd, cmdBuffer);
	}

	vk::endCommandBuffer(vkd, cmdBuffer);

	// Submit commands.
	vk::submitCommandsAndWait(vkd, device, queue, cmdBuffer);

	// Read result image aspects from the last used framebuffer.
	const tcu::UVec2	renderSize		(kFramebufferWidth, kFramebufferHeight);
	const auto			colorBuffer		= readColorAttachment(vkd, device, queue, queueIndex, allocator, colorImages.back()->get(), colorFormat, renderSize);
	const auto			depthBuffer		= readDepthAttachment(vkd, device, queue, queueIndex, allocator, dsImages.back()->get(), dsFormatInfo->imageFormat, renderSize);
	const auto			stencilBuffer	= readStencilAttachment(vkd, device, queue, queueIndex, allocator, dsImages.back()->get(), dsFormatInfo->imageFormat, renderSize, vk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	const auto			colorAccess		= colorBuffer->getAccess();
	const auto			depthAccess		= depthBuffer->getAccess();
	const auto			stencilAccess	= stencilBuffer->getAccess();

	const int kWidth	= static_cast<int>(kFramebufferWidth);
	const int kHeight	= static_cast<int>(kFramebufferHeight);

	// Generate reference color buffer.
	const auto				tcuColorFormat			= vk::mapVkFormat(colorFormat);
	tcu::TextureLevel		referenceColorLevel		(tcuColorFormat, kWidth, kHeight);
	tcu::PixelBufferAccess	referenceColorAccess	= referenceColorLevel.getAccess();
	(*m_testConfig.referenceColor)(referenceColorAccess);

	const tcu::TextureFormat	errorFormat			(tcu::TextureFormat::RGBA, tcu::TextureFormat::UNORM_INT8);
	tcu::TextureLevel			colorError			(errorFormat, kWidth, kHeight);
	tcu::TextureLevel			depthError			(errorFormat, kWidth, kHeight);
	tcu::TextureLevel			stencilError		(errorFormat, kWidth, kHeight);
	const auto					colorErrorAccess	= colorError.getAccess();
	const auto					depthErrorAccess	= depthError.getAccess();
	const auto					stencilErrorAccess	= stencilError.getAccess();
	const tcu::Vec4				kGood				(0.0f, 1.0f, 0.0f, 1.0f);
	const tcu::Vec4				kBad				(1.0f, 0.0f, 0.0f, 1.0f);

	// Check expected values.
	const auto	minDepth		= m_testConfig.expectedDepth - dsFormatInfo->depthThreshold;
	const auto	maxDepth		= m_testConfig.expectedDepth + dsFormatInfo->depthThreshold;
	bool		colorMatch		= true;
	bool		depthMatch		= true;
	bool		stencilMatch	= true;
	bool		match;

	for (int y = 0; y < kHeight; ++y)
	for (int x = 0; x < kWidth; ++x)
	{
		if (vk::isUnormFormat(colorFormat))
		{
			auto colorPixel		= colorAccess.getPixel(x, y);
			auto expectedPixel	= referenceColorAccess.getPixel(x, y);
			match = tcu::boolAll(tcu::lessThan(tcu::absDiff(colorPixel, expectedPixel), kUnormColorThreshold));
		}
		else
		{
			DE_ASSERT(vk::isUintFormat(colorFormat));
			auto colorPixel		= colorAccess.getPixelUint(x, y);
			auto expectedPixel	= referenceColorAccess.getPixelUint(x, y);
			match = (colorPixel == expectedPixel);
		}

		colorErrorAccess.setPixel((match ? kGood : kBad), x, y);
		if (!match)
			colorMatch = false;

		const auto depthPixel = depthAccess.getPixDepth(x, y);
		match = de::inRange(depthPixel, minDepth, maxDepth);
		depthErrorAccess.setPixel((match ? kGood : kBad), x, y);
		if (!match)
			depthMatch = false;

		const auto stencilPixel = static_cast<deUint32>(stencilAccess.getPixStencil(x, y));
		match = (stencilPixel == m_testConfig.expectedStencil);
		stencilErrorAccess.setPixel((match ? kGood : kBad), x, y);
		if (!match)
			stencilMatch = false;
	}

	if (!colorMatch)
		logErrors(log, "Color", "Result color image and error mask", colorAccess, colorErrorAccess);

	if (!depthMatch)
		logErrors(log, "Depth", "Result depth image and error mask", depthAccess, depthErrorAccess);

	if (!stencilMatch)
		logErrors(log, "Stencil", "Result stencil image and error mask", stencilAccess, stencilErrorAccess);

	if (!(colorMatch && depthMatch && stencilMatch))
		return tcu::TestStatus::fail("Incorrect value found in attachments; please check logged images");

	return tcu::TestStatus::pass("Pass");
}

bool stencilPasses(vk::VkCompareOp op, deUint8 storedValue, deUint8 referenceValue)
{
	switch (op)
	{
	case vk::VK_COMPARE_OP_NEVER:				return false;
	case vk::VK_COMPARE_OP_LESS:				return (referenceValue <	storedValue);
	case vk::VK_COMPARE_OP_EQUAL:				return (referenceValue ==	storedValue);
	case vk::VK_COMPARE_OP_LESS_OR_EQUAL:		return (referenceValue <=	storedValue);
	case vk::VK_COMPARE_OP_GREATER:				return (referenceValue >	storedValue);
	case vk::VK_COMPARE_OP_GREATER_OR_EQUAL:	return (referenceValue >=	storedValue);
	case vk::VK_COMPARE_OP_ALWAYS:				return true;
	default: DE_ASSERT(false); return false;
	}

	return false;	// Unreachable.
}

deUint8 stencilResult(vk::VkStencilOp op, deUint8 storedValue, deUint8 referenceValue, deUint8 min, deUint8 max)
{
	deUint8 result = storedValue;

	switch (op)
	{
	case vk::VK_STENCIL_OP_KEEP:					break;
	case vk::VK_STENCIL_OP_ZERO:					result = 0; break;
	case vk::VK_STENCIL_OP_REPLACE:					result = referenceValue; break;
	case vk::VK_STENCIL_OP_INCREMENT_AND_CLAMP:		result = ((result == max) ? result : static_cast<deUint8>(result + 1)); break;
	case vk::VK_STENCIL_OP_DECREMENT_AND_CLAMP:		result = ((result == min) ? result : static_cast<deUint8>(result - 1)); break;
	case vk::VK_STENCIL_OP_INVERT:					result = static_cast<deUint8>(~result); break;
	case vk::VK_STENCIL_OP_INCREMENT_AND_WRAP:		result = ((result == max) ? min : static_cast<deUint8>(result + 1)); break;
	case vk::VK_STENCIL_OP_DECREMENT_AND_WRAP:		result = ((result == min) ? max : static_cast<deUint8>(result - 1)); break;
	default: DE_ASSERT(false); break;
	}

	return result;
}

} // anonymous namespace

tcu::TestCaseGroup* createExtendedDynamicStateTests (tcu::TestContext& testCtx, vk::PipelineConstructionType pipelineConstructionType)
{
	de::MovePtr<tcu::TestCaseGroup> extendedDynamicStateGroup(new tcu::TestCaseGroup(testCtx, "extended_dynamic_state", "Tests for VK_EXT_extended_dynamic_state"));
	de::MovePtr<tcu::TestCaseGroup> meshShaderGroup(new tcu::TestCaseGroup(testCtx, "mesh_shader", "Extended dynamic state with mesh shading pipelines"));

	// Auxiliar constants.
	const deUint32	kHalfWidthU	= kFramebufferWidth/2u;
	const deInt32	kHalfWidthI	= static_cast<deInt32>(kHalfWidthU);
	const float		kHalfWidthF	= static_cast<float>(kHalfWidthU);
	const float		kHeightF	= static_cast<float>(kFramebufferHeight);

	static const struct
	{
		SequenceOrdering	ordering;
		std::string			name;
		std::string			desc;
	} kOrderingCases[] =
	{
		{ SequenceOrdering::CMD_BUFFER_START,	"cmd_buffer_start",		"Dynamic state set after command buffer start"																								},
		{ SequenceOrdering::BEFORE_DRAW,		"before_draw",			"Dynamic state set just before drawing"																										},
		{ SequenceOrdering::BETWEEN_PIPELINES,	"between_pipelines",	"Dynamic after a pipeline with static states has been bound and before a pipeline with dynamic states has been bound"						},
		{ SequenceOrdering::AFTER_PIPELINES,	"after_pipelines",		"Dynamic state set after both a static-state pipeline and a second dynamic-state pipeline have been bound"									},
		{ SequenceOrdering::BEFORE_GOOD_STATIC,	"before_good_static",	"Dynamic state set after a dynamic pipeline has been bound and before a second static-state pipeline with the right values has been bound"	},
		{ SequenceOrdering::TWO_DRAWS_DYNAMIC,	"two_draws_dynamic",	"Bind bad static pipeline and draw, followed by binding correct dynamic pipeline and drawing again"											},
		{ SequenceOrdering::TWO_DRAWS_STATIC,	"two_draws_static",		"Bind bad dynamic pipeline and draw, followed by binding correct static pipeline and drawing again"											},
	};

	static const struct
	{
		bool			useMeshShaders;
		std::string		groupName;
	} kMeshShadingCases[] =
	{
		{ false,	""				},
#ifndef CTS_USES_VULKANSC
		{ true,		"mesh_shader"	},
#endif // CTS_USES_VULKANSC
	};

	static const struct
	{
		bool			bindUnusedMeshShadingPipeline;
		std::string		nameSuffix;
		std::string		descSuffix;
	} kBindUnusedCases[] =
	{
		{ false,	"",					""																},
#ifndef CTS_USES_VULKANSC
		{ true,		"_bind_unused_ms",	" and bind unused mesh shading pipeline before the dynamic one"	},
#endif // CTS_USES_VULKANSC
	};

	for (const auto& kMeshShadingCase : kMeshShadingCases)
	for (const auto& kOrderingCase : kOrderingCases)
	{
		const auto& kUseMeshShaders	= kMeshShadingCase.useMeshShaders;
		const auto& kOrdering		= kOrderingCase.ordering;

		de::MovePtr<tcu::TestCaseGroup> orderingGroup(new tcu::TestCaseGroup(testCtx, kOrderingCase.name.c_str(), kOrderingCase.desc.c_str()));

		// Cull modes.
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			config.cullModeConfig.staticValue	= vk::VK_CULL_MODE_FRONT_BIT;
			config.cullModeConfig.dynamicValue	= tcu::just<vk::VkCullModeFlags>(vk::VK_CULL_MODE_NONE);
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "cull_none", "Dynamically set cull mode to none", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			config.cullModeConfig.staticValue	= vk::VK_CULL_MODE_FRONT_AND_BACK;
			config.cullModeConfig.dynamicValue	= tcu::just<vk::VkCullModeFlags>(vk::VK_CULL_MODE_BACK_BIT);
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "cull_back", "Dynamically set cull mode to back", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			// Make triangles look back.
			config.meshParams[0].reversed		= true;
			config.cullModeConfig.staticValue	= vk::VK_CULL_MODE_BACK_BIT;
			config.cullModeConfig.dynamicValue	= tcu::just<vk::VkCullModeFlags>(vk::VK_CULL_MODE_FRONT_BIT);
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "cull_front", "Dynamically set cull mode to front", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			config.cullModeConfig.staticValue	= vk::VK_CULL_MODE_NONE;
			config.cullModeConfig.dynamicValue	= tcu::just<vk::VkCullModeFlags>(vk::VK_CULL_MODE_FRONT_AND_BACK);
			config.referenceColor.reset			(new SingleColorGenerator(kDefaultClearColor));
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "cull_front_and_back", "Dynamically set cull mode to front and back", config));
		}

		// Front face.
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			config.cullModeConfig.staticValue	= vk::VK_CULL_MODE_BACK_BIT;
			config.frontFaceConfig.staticValue	= vk::VK_FRONT_FACE_CLOCKWISE;
			config.frontFaceConfig.dynamicValue	= tcu::just<vk::VkFrontFace>(vk::VK_FRONT_FACE_COUNTER_CLOCKWISE);
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "front_face_cw", "Dynamically set front face to clockwise", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			// Pass triangles in clockwise order.
			config.meshParams[0].reversed		= true;
			config.cullModeConfig.staticValue	= vk::VK_CULL_MODE_BACK_BIT;
			config.frontFaceConfig.staticValue	= vk::VK_FRONT_FACE_COUNTER_CLOCKWISE;
			config.frontFaceConfig.dynamicValue	= tcu::just<vk::VkFrontFace>(vk::VK_FRONT_FACE_CLOCKWISE);
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "front_face_ccw", "Dynamically set front face to counter-clockwise", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			config.cullModeConfig.staticValue	= vk::VK_CULL_MODE_BACK_BIT;
			config.frontFaceConfig.staticValue	= vk::VK_FRONT_FACE_COUNTER_CLOCKWISE;
			config.frontFaceConfig.dynamicValue	= tcu::just<vk::VkFrontFace>(vk::VK_FRONT_FACE_CLOCKWISE);
			config.referenceColor.reset			(new SingleColorGenerator(kDefaultClearColor));
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "front_face_cw_reversed", "Dynamically set front face to clockwise with a counter-clockwise mesh", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			// Pass triangles in clockwise order.
			config.meshParams[0].reversed		= true;
			config.cullModeConfig.staticValue	= vk::VK_CULL_MODE_BACK_BIT;
			config.frontFaceConfig.staticValue	= vk::VK_FRONT_FACE_CLOCKWISE;
			config.frontFaceConfig.dynamicValue	= tcu::just<vk::VkFrontFace>(vk::VK_FRONT_FACE_COUNTER_CLOCKWISE);
			config.referenceColor.reset			(new SingleColorGenerator(kDefaultClearColor));
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "front_face_ccw_reversed", "Dynamically set front face to counter-clockwise with a clockwise mesh", config));
		}

		// Rasterizer discard
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			config.rastDiscardEnableConfig.staticValue	= false;
			config.rastDiscardEnableConfig.dynamicValue	= tcu::just(true);
			config.referenceColor.reset					(new SingleColorGenerator(kDefaultClearColor));
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "disable_raster", "Dynamically disable rasterizer", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			config.rastDiscardEnableConfig.staticValue	= true;
			config.rastDiscardEnableConfig.dynamicValue	= tcu::just(false);
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "enable_raster", "Dynamically enable rasterizer", config));
		}

		// Logic op
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			config.logicOpConfig.staticValue	= vk::VK_LOGIC_OP_CLEAR;
			config.logicOpConfig.dynamicValue	= tcu::just<vk::VkLogicOp>(vk::VK_LOGIC_OP_OR);
			// Clear to green, paint in blue, expect cyan due to logic op.
			config.meshParams[0].color			= kLogicOpTriangleColor;
			config.clearColorValue				= vk::makeClearValueColorU32(kGreenClearColor.x(), kGreenClearColor.y(), kGreenClearColor.z(), kGreenClearColor.w());
			config.referenceColor.reset			(new SingleColorGenerator(kLogicOpFinalColor));
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "logic_op_or", "Dynamically change logic op to VK_LOGIC_OP_OR", config));
		}

		// Dynamically enable primitive restart
		if (!kUseMeshShaders)
		{
			const std::vector<std::string> flag2Enable { "disable", "enable" };

			for (const auto& bindUnusedCase : kBindUnusedCases)
			{
				if (bindUnusedCase.bindUnusedMeshShadingPipeline && kOrdering != SequenceOrdering::CMD_BUFFER_START)
					continue;

				for (int staticFlag = 0; staticFlag < 2; ++staticFlag)
				for (int dynamicFlag = 0; dynamicFlag < 2; ++dynamicFlag)
				{
					const bool staticValue	= (staticFlag > 0);
					const bool dynamicValue	= (dynamicFlag > 0);
					const auto topology		= vk::VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
					const auto dynAction	= flag2Enable[static_cast<int>(dynamicValue)];
					const auto nameAction	= dynAction + ((staticValue == dynamicValue) ? "_on_both" : "");
					const auto descAction	= dynAction + ((staticValue == dynamicValue) ? " (statically and dynamically)" : "");

					TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
					config.primRestartEnableConfig.staticValue	= staticValue;
					config.primRestartEnableConfig.dynamicValue	= tcu::just(dynamicValue);
					config.topologyConfig.staticValue			= topology;
					config.bindUnusedMeshShadingPipeline		= bindUnusedCase.bindUnusedMeshShadingPipeline;
					config.referenceColor.reset					(dynamicValue
																? static_cast<ReferenceColorGenerator*>(new LastSegmentMissingGenerator(kDefaultTriangleColor, kDefaultClearColor))
																: static_cast<ReferenceColorGenerator*>(new SingleColorGenerator(kDefaultTriangleColor)));
					orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "prim_restart_" + nameAction + bindUnusedCase.nameSuffix, "Dynamically " + descAction + " primitiveRestart" + bindUnusedCase.descSuffix, config));
				}
			}
		}

		// Dynamically change the number of primitive control points
		if (!kUseMeshShaders)
		{
			for (const auto& bindUnusedCase : kBindUnusedCases)
			{
				if (bindUnusedCase.bindUnusedMeshShadingPipeline && kOrdering != SequenceOrdering::CMD_BUFFER_START)
					continue;

				TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
				config.topologyConfig.staticValue = vk::VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
				config.patchControlPointsConfig.staticValue = 1;
				config.patchControlPointsConfig.dynamicValue = 3;
				config.bindUnusedMeshShadingPipeline = bindUnusedCase.bindUnusedMeshShadingPipeline;
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "patch_control_points" + bindUnusedCase.nameSuffix, "Dynamically change patch control points" + bindUnusedCase.descSuffix, config));
			}

			{
				TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
				config.topologyConfig.staticValue = vk::VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
				config.patchControlPointsConfig.staticValue = 1;
				config.patchControlPointsConfig.dynamicValue = 3;
				config.useExtraDynPCPPipeline = true;

				const auto testName	= "patch_control_points_extra_pipeline";
				const auto testDesc	= "Dynamically change patch control points and draw first with a pipeline using the state and no tessellation shaders";

				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, testName, testDesc, config));
			}
		}

		// Dynamic topology.
		if (!kUseMeshShaders)
		{
			TestConfig baseConfig(pipelineConstructionType, kOrdering, kUseMeshShaders);

			for (int i = 0; i < 2; ++i)
			{
				const bool forceGeometryShader = (i > 0);

				static const struct
				{
					vk::VkPrimitiveTopology staticVal;
					vk::VkPrimitiveTopology dynamicVal;
				} kTopologyCases[] =
				{
					{ vk::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,	vk::VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP	},
					{ vk::VK_PRIMITIVE_TOPOLOGY_LINE_LIST,		vk::VK_PRIMITIVE_TOPOLOGY_LINE_STRIP		},
					{ vk::VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,		vk::VK_PRIMITIVE_TOPOLOGY_PATCH_LIST		},
				};

				for (const auto& kTopologyCase : kTopologyCases)
				{
					for (const auto& bindUnusedCase : kBindUnusedCases)
					{
						if (bindUnusedCase.bindUnusedMeshShadingPipeline && kOrdering != SequenceOrdering::CMD_BUFFER_START)
							continue;

						TestConfig config(baseConfig);
						config.forceGeometryShader					= forceGeometryShader;
						config.topologyConfig.staticValue			= kTopologyCase.staticVal;
						config.topologyConfig.dynamicValue			= tcu::just<vk::VkPrimitiveTopology>(kTopologyCase.dynamicVal);
						config.patchControlPointsConfig.staticValue	= (config.needsTessellation() ? 3u : 1u);
						config.bindUnusedMeshShadingPipeline		= bindUnusedCase.bindUnusedMeshShadingPipeline;

						const std::string	className	= topologyClassName(getTopologyClass(config.topologyConfig.staticValue));
						const std::string	name		= "topology_" + className + (forceGeometryShader ? "_geom" : "") + bindUnusedCase.nameSuffix;
						const std::string	desc		= "Dynamically switch primitive topologies from the " + className + " class" + (forceGeometryShader ? " and use a geometry shader" : "") + bindUnusedCase.descSuffix;
						orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, name, desc, config));
					}
				}
			}
		}

		// Viewport.
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			// 2 scissors, bad static single viewport.
			config.scissorConfig.staticValue	= ScissorVec{vk::makeRect2D(0, 0, kHalfWidthU, kFramebufferHeight), vk::makeRect2D(kHalfWidthI, 0, kHalfWidthU, kFramebufferHeight)};
			config.viewportConfig.staticValue	= ViewportVec(1u, vk::makeViewport(kHalfWidthU, kFramebufferHeight));
			config.viewportConfig.dynamicValue	= ViewportVec{
				vk::makeViewport(0.0f, 0.0f, kHalfWidthF, kHeightF, 0.0f, 1.0f),
				vk::makeViewport(kHalfWidthF, 0.0f, kHalfWidthF, kHeightF, 0.0f, 1.0f),
			};
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "2_viewports", "Dynamically set 2 viewports", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			// Bad static reduced viewport.
			config.viewportConfig.staticValue	= ViewportVec(1u, vk::makeViewport(kHalfWidthU, kFramebufferHeight));
			config.viewportConfig.staticValue	= ViewportVec(1u, vk::makeViewport(kFramebufferWidth, kFramebufferHeight));
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "1_full_viewport", "Dynamically set viewport to cover full framebuffer", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			// 2 scissors (left half, right half), 2 reversed static viewports that need fixing (right, left).
			config.scissorConfig.staticValue	= ScissorVec{vk::makeRect2D(0, 0, kHalfWidthU, kFramebufferHeight), vk::makeRect2D(kHalfWidthI, 0, kHalfWidthU, kFramebufferHeight)};
			config.viewportConfig.staticValue	= ViewportVec{
				vk::makeViewport(kHalfWidthF, 0.0f, kHalfWidthF, kHeightF, 0.0f, 1.0f),	// Right.
				vk::makeViewport(0.0f, 0.0f, kHalfWidthF, kHeightF, 0.0f, 1.0f),		// Left.
			};
			config.viewportConfig.dynamicValue	= ViewportVec{config.viewportConfig.staticValue.back(), config.viewportConfig.staticValue.front()};
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "2_viewports_switch", "Dynamically switch the order with 2 viewports", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			// 2 scissors, reversed dynamic viewports that should result in no drawing taking place.
			config.scissorConfig.staticValue	= ScissorVec{vk::makeRect2D(0, 0, kHalfWidthU, kFramebufferHeight), vk::makeRect2D(kHalfWidthI, 0, kHalfWidthU, kFramebufferHeight)};
			config.viewportConfig.staticValue	= ViewportVec{
				vk::makeViewport(0.0f, 0.0f, kHalfWidthF, kHeightF, 0.0f, 1.0f),		// Left.
				vk::makeViewport(kHalfWidthF, 0.0f, kHalfWidthF, kHeightF, 0.0f, 1.0f),	// Right.
			};
			config.viewportConfig.dynamicValue	= ViewportVec{config.viewportConfig.staticValue.back(), config.viewportConfig.staticValue.front()};
			config.referenceColor.reset			(new SingleColorGenerator(kDefaultClearColor));
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "2_viewports_switch_clean", "Dynamically switch the order with 2 viewports resulting in clean image", config));
		}

		// Scissor.
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			// 2 viewports, bad static single scissor.
			config.viewportConfig.staticValue	= ViewportVec{
				vk::makeViewport(0.0f, 0.0f, kHalfWidthF, kHeightF, 0.0f, 1.0f),
				vk::makeViewport(kHalfWidthF, 0.0f, kHalfWidthF, kHeightF, 0.0f, 1.0f),
			};
			config.scissorConfig.staticValue	= ScissorVec(1u, vk::makeRect2D(kHalfWidthI, 0, kHalfWidthU, kFramebufferHeight));
			config.scissorConfig.dynamicValue	= ScissorVec{
				vk::makeRect2D(kHalfWidthU, kFramebufferHeight),
				vk::makeRect2D(kHalfWidthI, 0, kHalfWidthU, kFramebufferHeight),
			};
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "2_scissors", "Dynamically set 2 scissors", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			// 1 viewport, bad static single scissor.
			config.scissorConfig.staticValue	= ScissorVec(1u, vk::makeRect2D(kHalfWidthI, 0, kHalfWidthU, kFramebufferHeight));
			config.scissorConfig.dynamicValue	= ScissorVec(1u, vk::makeRect2D(kFramebufferWidth, kFramebufferHeight));
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "1_full_scissor", "Dynamically set scissor to cover full framebuffer", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			// 2 viewports, 2 reversed scissors that need fixing.
			config.viewportConfig.staticValue	= ViewportVec{
				vk::makeViewport(0.0f, 0.0f, kHalfWidthF, kHeightF, 0.0f, 1.0f),
				vk::makeViewport(kHalfWidthF, 0.0f, kHalfWidthF, kHeightF, 0.0f, 1.0f),
			};
			config.scissorConfig.staticValue	= ScissorVec{
				vk::makeRect2D(kHalfWidthI, 0, kHalfWidthU, kFramebufferHeight),
				vk::makeRect2D(kHalfWidthU, kFramebufferHeight),
			};
			config.scissorConfig.dynamicValue	= ScissorVec{config.scissorConfig.staticValue.back(), config.scissorConfig.staticValue.front()};
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "2_scissors_switch", "Dynamically switch the order with 2 scissors", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			// 2 viewports, 2 scissors switched to prevent drawing.
			config.viewportConfig.staticValue	= ViewportVec{
				vk::makeViewport(0.0f, 0.0f, kHalfWidthF, kHeightF, 0.0f, 1.0f),
				vk::makeViewport(kHalfWidthF, 0.0f, kHalfWidthF, kHeightF, 0.0f, 1.0f),
			};
			config.scissorConfig.staticValue	= ScissorVec{
				vk::makeRect2D(kHalfWidthU, kFramebufferHeight),
				vk::makeRect2D(kHalfWidthI, 0, kHalfWidthU, kFramebufferHeight),
			};
			config.scissorConfig.dynamicValue	= ScissorVec{config.scissorConfig.staticValue.back(), config.scissorConfig.staticValue.front()};
			config.referenceColor.reset			(new SingleColorGenerator(kDefaultClearColor));
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "2_scissors_switch_clean", "Dynamically switch the order with 2 scissors to avoid drawing", config));
		}

		// Stride.
		if (!kUseMeshShaders)
		{
			struct
			{
				const VertexGenerator*	factory;
				const std::string		prefix;
			} strideCases[] =
			{
				{ getVertexWithPaddingGenerator(),			"stride"		},
				{ getVertexWithExtraAttributesGenerator(),	"large_stride"	},
			};

			for (const auto& strideCase : strideCases)
			{
				const auto	factory			= strideCase.factory;
				const auto&	prefix			= strideCase.prefix;
				const auto	vertexStrides	= factory->getVertexDataStrides();
				StrideVec	halfStrides;

				halfStrides.reserve(vertexStrides.size());
				for (const auto& stride : vertexStrides)
					halfStrides.push_back(stride / 2u);

				if (factory == getVertexWithExtraAttributesGenerator() && kOrdering == SequenceOrdering::TWO_DRAWS_STATIC)
				{
					// This case is invalid because it breaks VUID-vkCmdBindVertexBuffers2EXT-pStrides-03363 due to the dynamic
					// stride being less than the extent of the binding for the second attribute.
					continue;
				}

				for (const auto& bindUnusedCase : kBindUnusedCases)
				{
					if (bindUnusedCase.bindUnusedMeshShadingPipeline && kOrdering != SequenceOrdering::CMD_BUFFER_START)
						continue;

					{
						TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders, factory);
						config.strideConfig.staticValue			= halfStrides;
						config.strideConfig.dynamicValue		= vertexStrides;
						config.bindUnusedMeshShadingPipeline	= bindUnusedCase.bindUnusedMeshShadingPipeline;
						orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, prefix + bindUnusedCase.nameSuffix, "Dynamically set stride" + bindUnusedCase.descSuffix, config));
					}
					{
						TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders, factory);
						config.strideConfig.staticValue			= halfStrides;
						config.strideConfig.dynamicValue		= vertexStrides;
						config.vertexDataOffset					= vertexStrides[0];
						config.bindUnusedMeshShadingPipeline	= bindUnusedCase.bindUnusedMeshShadingPipeline;
						orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, prefix + "_with_offset" + bindUnusedCase.nameSuffix, "Dynamically set stride using a nonzero vertex data offset" + bindUnusedCase.descSuffix, config));
					}
					{
						TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders, factory);
						config.strideConfig.staticValue			= halfStrides;
						config.strideConfig.dynamicValue		= vertexStrides;
						config.vertexDataOffset					= vertexStrides[0];
						config.vertexDataExtraBytes				= config.vertexDataOffset;
						config.bindUnusedMeshShadingPipeline	= bindUnusedCase.bindUnusedMeshShadingPipeline;

						// Make the mesh cover the top half only. If the implementation reads data outside the vertex values it may draw something to the bottom half.
						config.referenceColor.reset				(new HorizontalSplitGenerator(kDefaultTriangleColor, kDefaultClearColor));
						config.meshParams[0].scaleY				= 0.5f;
						config.meshParams[0].offsetY			= -0.5f;

						orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, prefix + "_with_offset_and_padding" + bindUnusedCase.nameSuffix, "Dynamically set stride using a nonzero vertex data offset and extra bytes" + bindUnusedCase.descSuffix, config));
					}
				}
			}

			// Dynamic stride of 0
			//
			// The "two_draws" variants are invalid because the non-zero vertex stride will cause out-of-bounds access
			// when drawing more than one vertex.
			if (kOrdering != SequenceOrdering::TWO_DRAWS_STATIC && kOrdering != SequenceOrdering::TWO_DRAWS_DYNAMIC)
			{
				TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders, getVertexWithExtraAttributesGenerator());
				config.strideConfig.staticValue		= config.getActiveVertexGenerator()->getVertexDataStrides();
				config.strideConfig.dynamicValue	= { 0 };
				config.vertexDataOffset				= 4;
				config.singleVertex					= true;
				config.singleVertexDrawCount		= 6;

				// Make the mesh cover the top half only. If the implementation reads data outside the vertex data it should read the
				// offscreen vertex and draw something in the bottom half.
				config.referenceColor.reset		(new HorizontalSplitGenerator(kDefaultTriangleColor, kDefaultClearColor));
				config.meshParams[0].scaleY		= 0.5f;
				config.meshParams[0].offsetY	= -0.5f;

				// Use strip scale to synthesize a strip from a vertex attribute which remains constant over the draw call.
				config.meshParams[0].stripScale = 1.0f;

				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "zero_stride_with_offset", "Dynamically set zero stride using a nonzero vertex data offset", config));
			}
		}

		// Depth test enable.
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			config.depthTestEnableConfig.staticValue	= false;
			config.depthTestEnableConfig.dynamicValue	= tcu::just(true);
			// By default, the depth test never passes when enabled.
			config.referenceColor.reset					(new SingleColorGenerator(kDefaultClearColor));
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_test_enable", "Dynamically enable depth test", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			config.depthTestEnableConfig.staticValue	= true;
			config.depthTestEnableConfig.dynamicValue	= tcu::just(false);
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_test_disable", "Dynamically disable depth test", config));
		}

		// Depth write enable.
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);

			// Enable depth test and set values so it passes.
			config.depthTestEnableConfig.staticValue	= true;
			config.depthCompareOpConfig.staticValue		= vk::VK_COMPARE_OP_LESS;
			config.clearDepthValue						= 0.5f;
			config.meshParams[0].depth					= 0.25f;

			// Enable writes and expect the mesh value.
			config.depthWriteEnableConfig.staticValue	= false;
			config.depthWriteEnableConfig.dynamicValue	= tcu::just(true);
			config.expectedDepth						= 0.25f;

			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_write_enable", "Dynamically enable writes to the depth buffer", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);

			// Enable depth test and set values so it passes.
			config.depthTestEnableConfig.staticValue	= true;
			config.depthCompareOpConfig.staticValue		= vk::VK_COMPARE_OP_LESS;
			config.clearDepthValue						= 0.5f;
			config.meshParams[0].depth					= 0.25f;

			// But disable writing dynamically and expect the clear value.
			config.depthWriteEnableConfig.staticValue	= true;
			config.depthWriteEnableConfig.dynamicValue	= tcu::just(false);
			config.expectedDepth						= 0.5f;

			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_write_disable", "Dynamically disable writes to the depth buffer", config));
		}

		// Depth bias enable with static or dynamic depth bias parameters.
		{
			const DepthBiasParams kAlternativeDepthBiasParams = { 2e7f, 0.25f };

			for (int dynamicBiasIter = 0; dynamicBiasIter < 2; ++dynamicBiasIter)
			{
				const bool useDynamicBias = (dynamicBiasIter > 0);

				{
					TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);

					// Enable depth test and write 1.0f
					config.depthTestEnableConfig.staticValue = true;
					config.depthWriteEnableConfig.staticValue = true;
					config.depthCompareOpConfig.staticValue = vk::VK_COMPARE_OP_ALWAYS;
					// Clear depth buffer to 0.25f
					config.clearDepthValue = 0.25f;
					// Write depth to 0.5f
					config.meshParams[0].depth = 0.5f;

					// Enable dynamic depth bias and expect the depth value to be clamped to 0.75f based on depthBiasConstantFactor and depthBiasClamp
					if (useDynamicBias)
					{
						config.depthBiasConfig.staticValue	= kNoDepthBiasParams;
						config.depthBiasConfig.dynamicValue	= kAlternativeDepthBiasParams;
					}
					else
					{
						config.depthBiasConfig.staticValue	= kAlternativeDepthBiasParams;
					}

					config.depthBiasEnableConfig.staticValue = false;
					config.depthBiasEnableConfig.dynamicValue = tcu::just(true);
					config.expectedDepth = 0.75f;

					std::string caseName = "depth_bias_enable";
					std::string caseDesc = "Dynamically enable the depth bias";

					if (useDynamicBias)
					{
						caseName += "_dynamic_bias_params";
						caseDesc += " and set the bias params dynamically";
					}

					orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, caseName, caseDesc, config));
				}
				{
					TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);

					// Enable depth test and write 1.0f
					config.depthTestEnableConfig.staticValue = true;
					config.depthWriteEnableConfig.staticValue = true;
					config.depthCompareOpConfig.staticValue = vk::VK_COMPARE_OP_ALWAYS;
					// Clear depth buffer to 0.25f
					config.clearDepthValue = 0.25f;
					// Write depth to 0.5f
					config.meshParams[0].depth = 0.5f;

					// Disable dynamic depth bias and expect the depth value to remain at 0.5f based on written value
					if (useDynamicBias)
					{
						config.depthBiasConfig.staticValue	= kNoDepthBiasParams;
						config.depthBiasConfig.dynamicValue	= kAlternativeDepthBiasParams;
					}
					else
					{
						config.depthBiasConfig.staticValue	= kAlternativeDepthBiasParams;
					}

					config.depthBiasEnableConfig.staticValue = true;
					config.depthBiasEnableConfig.dynamicValue = tcu::just(false);
					config.expectedDepth = 0.5f;

					std::string caseName = "depth_bias_disable";
					std::string caseDesc = "Dynamically disable the depth bias";

					if (useDynamicBias)
					{
						caseName += "_dynamic_bias_params";
						caseDesc += " and set the bias params dynamically";
					}

					orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, caseName, caseDesc, config));
				}
			}
		}

		// Depth compare op.
		{
			TestConfig baseConfig(pipelineConstructionType, kOrdering, kUseMeshShaders);
			const tcu::Vec4 kAlternativeColor				(0.0f, 0.0f, 0.5f, 1.0f);
			baseConfig.depthTestEnableConfig.staticValue	= true;
			baseConfig.depthWriteEnableConfig.staticValue	= true;
			baseConfig.depthCompareOpConfig.staticValue		= vk::VK_COMPARE_OP_NEVER;
			baseConfig.clearDepthValue						= 0.5f;

			{
				TestConfig config = baseConfig;
				config.depthCompareOpConfig.staticValue		= vk::VK_COMPARE_OP_ALWAYS;
				config.depthCompareOpConfig.dynamicValue	= vk::VK_COMPARE_OP_NEVER;
				config.meshParams[0].depth					= 0.25f;
				config.expectedDepth						= 0.5f;
				config.referenceColor.reset					(new SingleColorGenerator(kDefaultClearColor));
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_compare_never", "Dynamically set the depth compare operator to NEVER", config));
			}
			{
				TestConfig config = baseConfig;
				config.depthCompareOpConfig.dynamicValue	= vk::VK_COMPARE_OP_LESS;
				config.meshParams[0].depth					= 0.25f;
				config.expectedDepth						= 0.25f;
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_compare_less", "Dynamically set the depth compare operator to LESS", config));
			}
			{
				TestConfig config = baseConfig;
				config.depthCompareOpConfig.dynamicValue	= vk::VK_COMPARE_OP_GREATER;
				config.meshParams[0].depth					= 0.75f;
				config.expectedDepth						= 0.75f;
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_compare_greater", "Dynamically set the depth compare operator to GREATER", config));
			}
			{
				TestConfig config = baseConfig;
				config.depthCompareOpConfig.dynamicValue	= vk::VK_COMPARE_OP_EQUAL;
				config.meshParams[0].depth					= 0.5f;
				config.meshParams[0].color					= kAlternativeColor;
				// Draw another mesh in front to verify it does not pass the equality test.
				config.meshParams.push_back(MeshParams(kDefaultTriangleColor, 0.25f));
				config.expectedDepth						= 0.5f;
				config.referenceColor.reset					(new SingleColorGenerator(kAlternativeColor));
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_compare_equal", "Dynamically set the depth compare operator to EQUAL", config));
			}
			{
				TestConfig config = baseConfig;
				config.depthCompareOpConfig.dynamicValue	= vk::VK_COMPARE_OP_LESS_OR_EQUAL;
				config.meshParams[0].depth					= 0.25f;
				config.expectedDepth						= 0.25f;
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_compare_less_equal_less", "Dynamically set the depth compare operator to LESS_OR_EQUAL and draw with smaller depth", config));
			}
			{
				TestConfig config = baseConfig;
				config.depthCompareOpConfig.dynamicValue	= vk::VK_COMPARE_OP_LESS_OR_EQUAL;
				config.meshParams[0].depth					= 0.5f;
				config.expectedDepth						= 0.5f;
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_compare_less_equal_equal", "Dynamically set the depth compare operator to LESS_OR_EQUAL and draw with equal depth", config));
			}
			{
				TestConfig config = baseConfig;
				config.depthCompareOpConfig.dynamicValue	= vk::VK_COMPARE_OP_LESS_OR_EQUAL;
				config.meshParams[0].depth					= 0.25f;
				// Draw another mesh with the same depth in front of it.
				config.meshParams.push_back(MeshParams(kAlternativeColor, 0.25f));
				config.expectedDepth						= 0.25f;
				config.referenceColor.reset					(new SingleColorGenerator(kAlternativeColor));
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_compare_less_equal_less_then_equal", "Dynamically set the depth compare operator to LESS_OR_EQUAL and draw two meshes with less and equal depth", config));
			}
			{
				TestConfig config = baseConfig;
				config.depthCompareOpConfig.dynamicValue	= vk::VK_COMPARE_OP_GREATER_OR_EQUAL;
				config.meshParams[0].depth					= 0.75f;
				config.expectedDepth						= 0.75f;
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_compare_greater_equal_greater", "Dynamically set the depth compare operator to GREATER_OR_EQUAL and draw with greater depth", config));
			}
			{
				TestConfig config = baseConfig;
				config.depthCompareOpConfig.dynamicValue	= vk::VK_COMPARE_OP_GREATER_OR_EQUAL;
				config.meshParams[0].depth					= 0.5f;
				config.expectedDepth						= 0.5f;
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_compare_greater_equal_equal", "Dynamically set the depth compare operator to GREATER_OR_EQUAL and draw with equal depth", config));
			}
			{
				TestConfig config = baseConfig;
				config.depthCompareOpConfig.dynamicValue	= vk::VK_COMPARE_OP_GREATER_OR_EQUAL;
				config.meshParams[0].depth					= 0.75f;
				// Draw another mesh with the same depth in front of it.
				config.meshParams.push_back(MeshParams(kAlternativeColor, 0.75f));
				config.expectedDepth						= 0.75f;
				config.referenceColor.reset					(new SingleColorGenerator(kAlternativeColor));
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_compare_greater_equal_greater_then_equal", "Dynamically set the depth compare operator to GREATER_OR_EQUAL and draw two meshes with greater and equal depth", config));
			}
			{
				TestConfig config = baseConfig;
				config.depthCompareOpConfig.dynamicValue	= vk::VK_COMPARE_OP_NOT_EQUAL;

				// Draw first mesh in front.
				config.meshParams[0].depth					= 0.25f;
				// Draw another mesh in the back, this should pass too.
				config.meshParams.push_back(MeshParams(kAlternativeColor, 0.5f));
				// Finally a new mesh with the same depth. This should not pass.
				config.meshParams.push_back(MeshParams(kDefaultTriangleColor, 0.5f));

				config.referenceColor.reset					(new SingleColorGenerator(kAlternativeColor));
				config.expectedDepth						= 0.5f;
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_compare_not_equal", "Dynamically set the depth compare operator to NOT_EQUAL", config));
			}
			{
				TestConfig config = baseConfig;
				config.depthCompareOpConfig.dynamicValue	= vk::VK_COMPARE_OP_ALWAYS;

				config.meshParams[0].depth					= 0.5f;
				config.expectedDepth						= 0.5f;
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_compare_always_equal", "Dynamically set the depth compare operator to ALWAYS and draw with equal depth", config));

				config.meshParams[0].depth					= 0.25f;
				config.expectedDepth						= 0.25f;
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_compare_always_less", "Dynamically set the depth compare operator to ALWAYS and draw with less depth", config));

				config.meshParams[0].depth					= 0.75f;
				config.expectedDepth						= 0.75f;
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_compare_always_greater", "Dynamically set the depth compare operator to ALWAYS and draw with greater depth", config));
			}
		}

		// Depth bounds test.
		{
			TestConfig baseConfig(pipelineConstructionType, kOrdering, kUseMeshShaders);
			baseConfig.minDepthBounds							= 0.25f;
			baseConfig.maxDepthBounds							= 0.75f;
			baseConfig.meshParams[0].depth						= 0.0f;

			{
				TestConfig config = baseConfig;
				config.depthBoundsTestEnableConfig.staticValue	= false;
				config.depthBoundsTestEnableConfig.dynamicValue	= tcu::just(true);
				config.referenceColor.reset						(new SingleColorGenerator(kDefaultClearColor));
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_bounds_test_enable", "Dynamically enable the depth bounds test", config));
			}
			{
				TestConfig config = baseConfig;
				config.depthBoundsTestEnableConfig.staticValue	= true;
				config.depthBoundsTestEnableConfig.dynamicValue	= tcu::just(false);
				orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "depth_bounds_test_disable", "Dynamically disable the depth bounds test", config));
			}
		}

		// Stencil test enable.
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			config.stencilTestEnableConfig.staticValue				= false;
			config.stencilTestEnableConfig.dynamicValue				= tcu::just(true);
			config.stencilOpConfig.staticValue.front().compareOp	= vk::VK_COMPARE_OP_NEVER;
			config.referenceColor.reset								(new SingleColorGenerator(kDefaultClearColor));
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "stencil_test_enable", "Dynamically enable the stencil test", config));
		}
		{
			TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);
			config.stencilTestEnableConfig.staticValue				= true;
			config.stencilTestEnableConfig.dynamicValue				= tcu::just(false);
			config.stencilOpConfig.staticValue.front().compareOp	= vk::VK_COMPARE_OP_NEVER;
			orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "stencil_test_disable", "Dynamically disable the stencil test", config));
		}

		// Stencil operation. Many combinations are possible.
		{
			static const struct
			{
				vk::VkStencilFaceFlags	face;
				std::string				name;
			} kFaces[] =
			{
				{ vk::VK_STENCIL_FACE_FRONT_BIT,			"face_front"		},
				{ vk::VK_STENCIL_FACE_BACK_BIT,				"face_back"			},
				{ vk::VK_STENCIL_FACE_FRONT_AND_BACK,		"face_both_single"	},
				{ vk::VK_STENCIL_FACE_FLAG_BITS_MAX_ENUM,	"face_both_dual"	},	// MAX_ENUM is a placeholder.
			};

			static const struct
			{
				vk::VkCompareOp		compareOp;
				std::string			name;
			} kCompare[] =
			{
				{ vk::VK_COMPARE_OP_NEVER,				"xf"		},
				{ vk::VK_COMPARE_OP_LESS,				"lt"		},
				{ vk::VK_COMPARE_OP_EQUAL,				"eq"		},
				{ vk::VK_COMPARE_OP_LESS_OR_EQUAL,		"le"		},
				{ vk::VK_COMPARE_OP_GREATER,			"gt"		},
				{ vk::VK_COMPARE_OP_GREATER_OR_EQUAL,	"ge"		},
				{ vk::VK_COMPARE_OP_ALWAYS,				"xt"		},
			};

			using u8vec = std::vector<deUint8>;

			static const auto kMinVal	= std::numeric_limits<deUint8>::min();
			static const auto kMaxVal	= std::numeric_limits<deUint8>::max();
			static const auto kMidVal	= static_cast<deUint8>(kMaxVal * 2u / 5u);
			static const auto kMinValI	= static_cast<int>(kMinVal);
			static const auto kMaxValI	= static_cast<int>(kMaxVal);

			static const struct
			{
				vk::VkStencilOp		stencilOp;
				std::string			name;
				u8vec				clearValues;	// One test per clear value interesting for this operation.
				vk::VkStencilOp		incompatibleOp;	// Alternative operation giving incompatible results for the given values.
			} kStencilOps[] =
			{
				{ vk::VK_STENCIL_OP_KEEP,					"keep",			u8vec{kMidVal},					vk::VK_STENCIL_OP_ZERO					},
				{ vk::VK_STENCIL_OP_ZERO,					"zero",			u8vec{kMidVal},					vk::VK_STENCIL_OP_KEEP					},
				{ vk::VK_STENCIL_OP_REPLACE,				"replace",		u8vec{kMidVal},					vk::VK_STENCIL_OP_ZERO					},
				{ vk::VK_STENCIL_OP_INCREMENT_AND_CLAMP,	"inc_clamp",	u8vec{kMaxVal - 1, kMaxVal},	vk::VK_STENCIL_OP_ZERO					},
				{ vk::VK_STENCIL_OP_DECREMENT_AND_CLAMP,	"dec_clamp",	u8vec{kMinVal + 1, kMinVal},	vk::VK_STENCIL_OP_INCREMENT_AND_CLAMP	},
				{ vk::VK_STENCIL_OP_INVERT,					"invert",		u8vec{kMidVal},					vk::VK_STENCIL_OP_ZERO					},
				{ vk::VK_STENCIL_OP_INCREMENT_AND_WRAP,		"inc_wrap",		u8vec{kMaxVal - 1, kMaxVal},	vk::VK_STENCIL_OP_KEEP					},
				{ vk::VK_STENCIL_OP_DECREMENT_AND_WRAP,		"dec_wrap",		u8vec{kMinVal + 1, kMinVal},	vk::VK_STENCIL_OP_KEEP					},
			};

			for (const auto& face : kFaces)
			for (const auto& compare : kCompare)
			for (const auto& op : kStencilOps)
			{
				// Try clearing the stencil value with different values.
				for (const auto clearVal : op.clearValues)
				{
					// Use interesting values as the reference stencil value.
					for (int delta = -1; delta <= 1; ++delta)
					{
						const int refVal = clearVal + delta;
						if (refVal < kMinValI || refVal > kMaxValI)
							continue;

						const auto refValU8		= static_cast<deUint8>(refVal);
						const auto refValU32	= static_cast<deUint32>(refVal);

						// Calculate outcome of the stencil test itself.
						const bool wouldPass = stencilPasses(compare.compareOp, clearVal, refValU8);

						// If the test passes, use an additional variant for the depthFail operation.
						const int subCases = (wouldPass ? 2 : 1);

						for (int subCaseIdx = 0; subCaseIdx < subCases; ++subCaseIdx)
						{
							const bool depthFail	= (subCaseIdx > 0);				// depthFail would be the second variant.
							const bool globalPass	= (wouldPass && !depthFail);	// Global result of the stencil+depth test.

							// Start tuning test parameters.
							TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders);

							// No face culling is applied by default, so both the front and back operations could apply depending on the mesh.
							if (face.face == vk::VK_STENCIL_FACE_FRONT_BIT)
							{
								// Default parameters are OK.
							}
							else if (face.face == vk::VK_STENCIL_FACE_BACK_BIT)
							{
								// Reverse the mesh so it applies the back operation.
								config.meshParams[0].reversed = true;
							}
							else	// Front and back.
							{
								// Draw both a front and a back-facing mesh so both are applied.
								// The first mesh will be drawn in the top half and the second mesh in the bottom half.

								// Make the second mesh a reversed copy of the first mesh.
								config.meshParams.push_back(config.meshParams.front());
								config.meshParams.back().reversed = true;

								// Apply scale and offset to the top mesh.
								config.meshParams.front().scaleY = 0.5f;
								config.meshParams.front().offsetY = -0.5f;

								// Apply scale and offset to the bottom mesh.
								config.meshParams.back().scaleY = 0.5f;
								config.meshParams.back().offsetY = 0.5f;
							}

							// Enable the stencil test.
							config.stencilTestEnableConfig.staticValue = true;

							// Set dynamic configuration.
							StencilOpParams dynamicStencilConfig;
							dynamicStencilConfig.faceMask		= face.face;
							dynamicStencilConfig.compareOp		= compare.compareOp;
							dynamicStencilConfig.failOp			= vk::VK_STENCIL_OP_MAX_ENUM;
							dynamicStencilConfig.passOp			= vk::VK_STENCIL_OP_MAX_ENUM;
							dynamicStencilConfig.depthFailOp	= vk::VK_STENCIL_OP_MAX_ENUM;

							// Set operations so only the appropriate operation for this case gives the right result.
							vk::VkStencilOp* activeOp		= nullptr;
							vk::VkStencilOp* inactiveOps[2]	= { nullptr, nullptr };
							if (wouldPass)
							{
								if (depthFail)
								{
									activeOp		= &dynamicStencilConfig.depthFailOp;
									inactiveOps[0]	= &dynamicStencilConfig.passOp;
									inactiveOps[1]	= &dynamicStencilConfig.failOp;
								}
								else
								{
									activeOp		= &dynamicStencilConfig.passOp;
									inactiveOps[0]	= &dynamicStencilConfig.depthFailOp;
									inactiveOps[1]	= &dynamicStencilConfig.failOp;
								}
							}
							else
							{
								activeOp		= &dynamicStencilConfig.failOp;
								inactiveOps[0]	= &dynamicStencilConfig.passOp;
								inactiveOps[1]	= &dynamicStencilConfig.depthFailOp;
							}

							*activeOp = op.stencilOp;
							*inactiveOps[0] = op.incompatibleOp;
							*inactiveOps[1] = op.incompatibleOp;

							// Make sure all ops have been configured properly.
							DE_ASSERT(dynamicStencilConfig.failOp != vk::VK_STENCIL_OP_MAX_ENUM);
							DE_ASSERT(dynamicStencilConfig.passOp != vk::VK_STENCIL_OP_MAX_ENUM);
							DE_ASSERT(dynamicStencilConfig.depthFailOp != vk::VK_STENCIL_OP_MAX_ENUM);

							// Set an incompatible static operation too.
							auto& staticStencilConfig		= config.stencilOpConfig.staticValue.front();
							staticStencilConfig.faceMask	= face.face;
							staticStencilConfig.compareOp	= (globalPass ? vk::VK_COMPARE_OP_NEVER : vk::VK_COMPARE_OP_ALWAYS);
							staticStencilConfig.passOp		= op.incompatibleOp;
							staticStencilConfig.failOp		= op.incompatibleOp;
							staticStencilConfig.depthFailOp	= op.incompatibleOp;

							// Set dynamic configuration.
							StencilOpVec stencilOps;
							stencilOps.push_back(dynamicStencilConfig);

							if (stencilOps.front().faceMask == vk::VK_STENCIL_FACE_FLAG_BITS_MAX_ENUM)
							{
								// This is the dual case. We will set the front and back face values with two separate calls.
								stencilOps.push_back(stencilOps.front());
								stencilOps.front().faceMask		= vk::VK_STENCIL_FACE_FRONT_BIT;
								stencilOps.back().faceMask		= vk::VK_STENCIL_FACE_BACK_BIT;
								staticStencilConfig.faceMask	= vk::VK_STENCIL_FACE_FRONT_AND_BACK;
							}

							config.stencilOpConfig.dynamicValue	= tcu::just(stencilOps);
							config.clearStencilValue			= clearVal;
							config.referenceStencil				= refValU32;

							if (depthFail)
							{
								// Enable depth test and make it fail.
								config.depthTestEnableConfig.staticValue	= true;
								config.clearDepthValue						= 0.5f;
								config.depthCompareOpConfig.staticValue		= vk::VK_COMPARE_OP_LESS;

								for (auto& meshPar : config.meshParams)
									meshPar.depth = 0.75f;
							}

							// Set expected outcome.
							config.referenceColor.reset	(new SingleColorGenerator(globalPass ? kDefaultTriangleColor : kDefaultClearColor));
							config.expectedDepth		= config.clearDepthValue; // No depth writing by default.
							config.expectedStencil		= stencilResult(op.stencilOp, clearVal, refValU8, kMinVal, kMaxVal);

							const std::string testName = std::string("stencil_state")
								+ "_" + face.name
								+ "_" + compare.name
								+ "_" + op.name
								+ "_clear_" + de::toString(static_cast<int>(clearVal))
								+ "_ref_" + de::toString(refVal)
								+ "_" + (wouldPass ? (depthFail ? "depthfail" : "pass") : "fail");

							orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, testName, "Dynamically configure stencil test, variant " + testName, config));
						}
					}
				}
			}
		}

		// Vertex input.
		if (!kUseMeshShaders)
		{
			for (const auto& bindUnusedCase : kBindUnusedCases)
			{
				if (bindUnusedCase.bindUnusedMeshShadingPipeline && kOrdering != SequenceOrdering::CMD_BUFFER_START)
					continue;

				// TWO_DRAWS_STATIC would be invalid because it violates VUID-vkCmdBindVertexBuffers2EXT-pStrides-03363 due to the
				// dynamic stride being less than the extent of the binding for the second attribute.
				if (kOrdering != SequenceOrdering::TWO_DRAWS_STATIC)
				{
					const auto	staticGen	= getVertexWithPaddingGenerator();
					const auto	dynamicGen	= getVertexWithExtraAttributesGenerator();
					const auto	goodStrides	= dynamicGen->getVertexDataStrides();
					StrideVec	badStrides;

					badStrides.reserve(goodStrides.size());
					for (const auto& stride : goodStrides)
						badStrides.push_back(stride / 2u);

					TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders, staticGen, dynamicGen);
					config.strideConfig.staticValue			= badStrides;
					config.strideConfig.dynamicValue		= goodStrides;
					config.bindUnusedMeshShadingPipeline	= bindUnusedCase.bindUnusedMeshShadingPipeline;
					orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "vertex_input" + bindUnusedCase.nameSuffix, "Dynamically set vertex input" + bindUnusedCase.descSuffix, config));
				}

				{
					// Variant without mixing in the stride config.
					TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders, getVertexWithPaddingGenerator(), getVertexWithExtraAttributesGenerator());
					config.bindUnusedMeshShadingPipeline = bindUnusedCase.bindUnusedMeshShadingPipeline;
					orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "vertex_input_no_dyn_stride" + bindUnusedCase.nameSuffix, "Dynamically set vertex input without using dynamic strides" + bindUnusedCase.descSuffix, config));
				}

				{
					// Variant using multiple bindings.
					TestConfig config(pipelineConstructionType, kOrdering, kUseMeshShaders, getVertexWithExtraAttributesGenerator(), getVertexWithMultipleBindingsGenerator());
					config.bindUnusedMeshShadingPipeline = bindUnusedCase.bindUnusedMeshShadingPipeline;
					orderingGroup->addChild(new ExtendedDynamicStateTest(testCtx, "vertex_input_multiple_bindings" + bindUnusedCase.nameSuffix, "Dynamically set vertex input with multiple bindings" + bindUnusedCase.descSuffix, config));
				}
			}
		}

		tcu::TestCaseGroup* group = (kUseMeshShaders ? meshShaderGroup.get() : extendedDynamicStateGroup.get());
		group->addChild(orderingGroup.release());
	}

	extendedDynamicStateGroup->addChild(meshShaderGroup.release());
	return extendedDynamicStateGroup.release();
}

} // pipeline
} // vkt
