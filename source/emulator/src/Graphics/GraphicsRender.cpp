#include "Emulator/Graphics/GraphicsRender.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"
#include "Kyty/Math/Rand.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Objects/DepthStencilBuffer.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/RenderTexture.h"
#include "Emulator/Graphics/Objects/StorageBuffer.h"
#include "Emulator/Graphics/Objects/StorageTexture.h"
#include "Emulator/Graphics/Objects/Texture.h"
#include "Emulator/Graphics/Objects/VertexBuffer.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Profiler.h"

#include <atomic>
#include <vulkan/vulkan_core.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

constexpr int GRAPHICS_EVENT_EOP = 0x40;

struct Label;
struct RenderDepthInfo;

struct VulkanDescriptor
{
	VkDescriptorSet descriptor_set = nullptr;
};

#pragma pack(push, 1)

struct PipelineStencilStaticState
{
	VkStencilOp failOp      = VK_STENCIL_OP_KEEP;
	VkStencilOp passOp      = VK_STENCIL_OP_KEEP;
	VkStencilOp depthFailOp = VK_STENCIL_OP_KEEP;
	VkCompareOp compareOp   = VK_COMPARE_OP_NEVER;
};

struct PipelineStencilDynamicState
{
	uint32_t compareMask = 0;
	uint32_t writeMask   = 0;
	uint32_t reference   = 0;
};

struct PipelineStaticParameters
{
	float                      viewport_scale[3]        = {};
	float                      viewport_offset[3]       = {};
	int                        scissor_ltrb[4]          = {0};
	VkPrimitiveTopology        topology                 = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	bool                       with_depth               = false;
	bool                       depth_test_enable        = false;
	bool                       depth_write_enable       = false;
	VkCompareOp                depth_compare_op         = VK_COMPARE_OP_NEVER;
	bool                       depth_bounds_test_enable = false;
	float                      depth_min_bounds         = 0.0f;
	float                      depth_max_bounds         = 0.0f;
	bool                       stencil_test_enable      = false;
	PipelineStencilStaticState stencil_front;
	PipelineStencilStaticState stencil_back;
	uint32_t                   color_mask           = 0;
	bool                       cull_front           = false;
	bool                       cull_back            = false;
	bool                       face                 = false;
	uint8_t                    color_srcblend       = 0;
	uint8_t                    color_comb_fcn       = 0;
	uint8_t                    color_destblend      = 0;
	uint8_t                    alpha_srcblend       = 0;
	uint8_t                    alpha_comb_fcn       = 0;
	uint8_t                    alpha_destblend      = 0;
	bool                       separate_alpha_blend = false;
	bool                       blend_enable         = false;
	float                      blend_color_red      = 0.0f;
	float                      blend_color_green    = 0.0f;
	float                      blend_color_blue     = 0.0f;
	float                      blend_color_alpha    = 0.0f;

	bool operator==(const PipelineStaticParameters& other) const;
};

struct PipelineDynamicParameters
{
	bool vk_dynamic_state_stencil_compare_mask = false;
	bool vk_dynamic_state_stencil_write_mask   = false;
	bool vk_dynamic_state_stencil_reference    = false;

	PipelineStencilDynamicState stencil_front;
	PipelineStencilDynamicState stencil_back;

	bool operator==(const PipelineDynamicParameters& other) const;
};
#pragma pack(pop)

struct PipelineAdditionalParameters
{
	ShaderBindParameters vs_bind;
	ShaderBindParameters ps_bind;
	ShaderBindParameters cs_bind;
};

struct VulkanPipeline
{
	VkPipelineLayout                    pipeline_layout   = nullptr;
	VkPipeline                          pipeline          = nullptr;
	const PipelineStaticParameters*     static_params     = nullptr;
	PipelineDynamicParameters*          dynamic_params    = nullptr;
	const PipelineAdditionalParameters* additional_params = nullptr;
};

class PipelineCache
{
public:
	PipelineCache() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~PipelineCache() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(PipelineCache);

	VulkanPipeline* CreatePipeline(VulkanFramebuffer* framebuffer, RenderColorInfo* color, RenderDepthInfo* depth,
	                               const ShaderVertexInputInfo* vs_input_info, const VertexShaderInfo* vs_regs,
	                               const ShaderPixelInputInfo* ps_input_info, const PixelShaderInfo* ps_regs, VkPrimitiveTopology topology,
	                               uint32_t color_mask, const ModeControl& mc, const BlendControl& bc, const BlendColor& bclr,
	                               const ScreenViewport& vp);
	VulkanPipeline* CreatePipeline(const ShaderComputeInputInfo* input_info, const ComputeShaderInfo* cs_regs);
	void            DeletePipeline(VulkanPipeline* pipeline);
	void            DeletePipelines(VulkanFramebuffer* framebuffer);
	void            DeleteAllPipelines();

private:
	static constexpr uint32_t MAX_PIPELINES = 32;

	struct Pipeline
	{
		uint64_t                   render_pass_id = 0;
		ShaderId                   vs_shader_id;
		ShaderId                   ps_shader_id;
		ShaderId                   cs_shader_id;
		VulkanPipeline*            pipeline       = nullptr;
		PipelineStaticParameters*  static_params  = nullptr;
		PipelineDynamicParameters* dynamic_params = nullptr;
	};

	[[nodiscard]] VulkanPipeline* Find(const Pipeline& p) const;

	static void DeletePipelineInternal(Pipeline& p);

	Vector<Pipeline> m_pipelines;
	Core::Mutex      m_mutex;
};

struct VulkanDescriptorSet
{
	VkDescriptorSet       set    = nullptr;
	VkDescriptorPool      pool   = nullptr;
	VkDescriptorSetLayout layout = nullptr;
};

class DescriptorCache
{
public:
	enum class Stage
	{
		Unknown,
		Vertex,
		Pixel,
		Compute
	};

	static constexpr int BUFFERS_MAX          = ShaderStorageResources::BUFFERS_MAX;
	static constexpr int TEXTURES_SAMPLED_MAX = ShaderTextureResources::RES_MAX;
	static constexpr int TEXTURES_STORAGE_MAX = ShaderTextureResources::RES_MAX;
	static constexpr int SAMPLERS_MAX         = ShaderSamplerResources::RES_MAX;
	static constexpr int PUSH_CONSTANTS_MAX   = 16 * 4;
	static constexpr int GDS_BUFFER_MAX       = 1;

	DescriptorCache() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~DescriptorCache() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(DescriptorCache);

	VkDescriptorSetLayout GetDescriptorSetLayout(Stage stage, const ShaderBindResources& bind, const ShaderBindParameters& bind_params);

	VulkanDescriptorSet* Allocate(Stage stage, int storage_buffers_num, int textures2d_sampled_num, int textures2d_storage_num,
	                              int samplers_num, int gds_buffers_num);
	void                 Free(VulkanDescriptorSet* set);

	VulkanDescriptorSet* GetDescriptor(Stage stage, VulkanBuffer** storage_buffers, VulkanImage** textures2d_sampled,
	                                   VulkanImage** textures2d_storage, uint64_t* samplers, VulkanBuffer** gds_buffers,
	                                   const ShaderBindResources& bind, const ShaderBindParameters& bind_params);
	void                 FreeDescriptor(VulkanBuffer* buffer);
	void                 FreeDescriptor(VulkanImage* image);

private:
	struct Set
	{
		VulkanDescriptorSet* set                                         = nullptr;
		Stage                stage                                       = Stage::Unknown;
		int                  storage_buffers_num                         = 0;
		uint64_t             storage_buffers_id[BUFFERS_MAX]             = {};
		int                  textures2d_sampled_num                      = 0;
		uint64_t             textures2d_sampled_id[TEXTURES_SAMPLED_MAX] = {};
		int                  textures2d_storage_num                      = 0;
		uint64_t             textures2d_storage_id[TEXTURES_STORAGE_MAX] = {};
		int                  samplers_num                                = 0;
		uint64_t             samplers_id[SAMPLERS_MAX]                   = {};
		int                  gds_buffers_num                             = 0;
		uint64_t             gds_buffers_id[GDS_BUFFER_MAX]              = {};
	};
	void Init();
	void CreatePool();

	Core::Mutex              m_mutex;
	Vector<VkDescriptorPool> m_pools;
	Vector<Set>              m_sets;
	VkDescriptorSetLayout    m_descriptor_set_layout_vertex[BUFFERS_MAX + 1][TEXTURES_SAMPLED_MAX + 1][TEXTURES_STORAGE_MAX + 1]
	                                                    [SAMPLERS_MAX + 1][GDS_BUFFER_MAX + 1] = {};
	VkDescriptorSetLayout m_descriptor_set_layout_pixel[BUFFERS_MAX + 1][TEXTURES_SAMPLED_MAX + 1][TEXTURES_STORAGE_MAX + 1]
	                                                   [SAMPLERS_MAX + 1][GDS_BUFFER_MAX + 1] = {};
	VkDescriptorSetLayout m_descriptor_set_layout_compute[BUFFERS_MAX + 1][TEXTURES_SAMPLED_MAX + 1][TEXTURES_STORAGE_MAX + 1]
	                                                     [SAMPLERS_MAX + 1][GDS_BUFFER_MAX + 1] = {};
	bool m_initialized                                                                          = false;
};

class SamplerCache
{
public:
	SamplerCache() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~SamplerCache() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(SamplerCache);

	VkSampler GetSampler(uint64_t id);
	uint64_t  GetSamplerId(const ShaderSamplerResource& r);

private:
	struct Sampler
	{
		ShaderSamplerResource r;
		VkSampler             vk = nullptr;
	};

	Core::Mutex     m_mutex;
	Vector<Sampler> m_samplers;
};

struct VulkanFramebuffer
{
	VkRenderPass  render_pass    = nullptr;
	uint64_t      render_pass_id = 0;
	VkFramebuffer framebuffer    = nullptr;
};

class FramebufferCache
{
public:
	FramebufferCache() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~FramebufferCache() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(FramebufferCache);

	VulkanFramebuffer* CreateFramebuffer(RenderColorInfo* color, RenderDepthInfo* depth);
	void               FreeFramebufferByColor(VulkanImage* image);
	void               FreeFramebufferByDepth(DepthStencilVulkanImage* image);

private:
	VideoOutVulkanImage* CreateDummyBuffer(VkFormat format, uint32_t width, uint32_t height);

	struct Framebuffer
	{
		VulkanFramebuffer* framebuffer          = nullptr;
		uint64_t           image_id             = 0;
		uint64_t           depth_id             = 0;
		bool               depth_clear_enable   = false;
		bool               stencil_clear_enable = false;
	};

	Core::Mutex                  m_mutex;
	Vector<Framebuffer>          m_framebuffers;
	Vector<VideoOutVulkanImage*> m_dummy_buffers;
};

class GdsBuffer
{
public:
	GdsBuffer() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~GdsBuffer() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(GdsBuffer);

	void Clear(GraphicContext* ctx, uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value);
	void Read(GraphicContext* ctx, uint32_t* dst, uint32_t dw_offset, uint32_t dw_size);

	VulkanBuffer* GetBuffer(GraphicContext* ctx);

private:
	static constexpr uint64_t DW_SIZE = 0x3000;

	void Init(GraphicContext* ctx);

	Core::Mutex   m_mutex;
	VulkanBuffer* m_buffer = nullptr;
};

class RenderContext
{
public:
	RenderContext()
	    : m_pipeline_cache(new PipelineCache), m_descriptor_cache(new DescriptorCache), m_framebuffer_cache(new FramebufferCache),
	      m_sampler_cache(new SamplerCache), m_gds_buffer(new GdsBuffer)
	{
		EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
	}
	virtual ~RenderContext() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(RenderContext);

	void            SetGraphicCtx(GraphicContext* ctx) { m_graphic_ctx = ctx; }
	GraphicContext* GetGraphicCtx() { return m_graphic_ctx; }

	Core::Mutex&      GetMutex() { return m_mutex; }
	PipelineCache*    GetPipelineCache() { return m_pipeline_cache; }
	DescriptorCache*  GetDescriptorCache() { return m_descriptor_cache; }
	FramebufferCache* GetFramebufferCache() { return m_framebuffer_cache; }
	SamplerCache*     GetSamplerCache() { return m_sampler_cache; }
	GdsBuffer*        GetGdsBuffer() { return m_gds_buffer; }

	//[[nodiscard]] LibKernel::EventQueue::KernelEqueue GetEopEq() const { return m_eop_eq; }
	// void                                              SetEopEq(LibKernel::EventQueue::KernelEqueue eop_eq) { m_eop_eq = eop_eq; }
	void AddEopEq(LibKernel::EventQueue::KernelEqueue eq);
	void DeleteEopEq(LibKernel::EventQueue::KernelEqueue eq);
	void TriggerEopEvent();

private:
	Core::Mutex       m_mutex;
	PipelineCache*    m_pipeline_cache    = nullptr;
	DescriptorCache*  m_descriptor_cache  = nullptr;
	FramebufferCache* m_framebuffer_cache = nullptr;
	SamplerCache*     m_sampler_cache     = nullptr;
	GraphicContext*   m_graphic_ctx       = nullptr;
	GdsBuffer*        m_gds_buffer        = nullptr;

	Core::Mutex                                 m_eop_mutex;
	Vector<LibKernel::EventQueue::KernelEqueue> m_eop_eqs;
};

struct RenderDepthInfo
{
	VkFormat                    format                   = VK_FORMAT_UNDEFINED;
	uint32_t                    width                    = 0;
	uint32_t                    height                   = 0;
	bool                        htile                    = false;
	bool                        neo                      = false;
	uint64_t                    depth_buffer_size        = 0;
	uint64_t                    depth_buffer_vaddr       = 0;
	uint64_t                    stencil_buffer_size      = 0;
	uint64_t                    stencil_buffer_vaddr     = 0;
	uint64_t                    htile_buffer_size        = 0;
	uint64_t                    htile_buffer_vaddr       = 0;
	bool                        depth_clear_enable       = false;
	float                       depth_clear_value        = 0.0f;
	bool                        depth_test_enable        = false;
	bool                        depth_write_enable       = false;
	VkCompareOp                 depth_compare_op         = VK_COMPARE_OP_NEVER;
	bool                        depth_bounds_test_enable = false;
	float                       depth_min_bounds         = 0.0f;
	float                       depth_max_bounds         = 0.0f;
	bool                        stencil_clear_enable     = false;
	uint8_t                     stencil_clear_value      = 0;
	bool                        stencil_test_enable      = false;
	PipelineStencilStaticState  stencil_static_front;
	PipelineStencilStaticState  stencil_static_back;
	PipelineStencilDynamicState stencil_dynamic_front;
	PipelineStencilDynamicState stencil_dynamic_back;
	DepthStencilVulkanImage*    vulkan_buffer = nullptr;
	uint64_t                    vaddr[3]      = {};
	uint64_t                    size[3]       = {};
	int                         vaddr_num     = 0;
};

enum class RenderColorType
{
	NoColorOutput,
	DisplayBuffer,
	OffscreenBuffer,
	RenderTexture,
};

struct RenderColorInfo
{
	RenderColorType type          = RenderColorType::NoColorOutput;
	VulkanImage*    vulkan_buffer = nullptr;
	uint64_t        base_addr     = 0;
	uint64_t        buffer_size   = 0;
};

class CommandPool
{
public:
	CommandPool() = default;
	~CommandPool()
	{
		if (m_pool != nullptr)
		{
			// TODO(): check if destructor is called from std::_Exit()
			// Delete();
		}
	}

	KYTY_CLASS_NO_COPY(CommandPool);

	VulkanCommandPool* GetPool()
	{
		if (m_pool == nullptr)
		{
			Create();
		}
		return m_pool;
	}

private:
	void Create();
	void Delete();

	VulkanCommandPool* m_pool = nullptr;
};

static RenderContext*           g_render_ctx = nullptr;
static thread_local CommandPool g_command_pool;

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void uc_print(const char* func, const UserConfig& uc)
{
	printf("%s\n", func);

	printf("\t GetPrimType()  = 0x%08" PRIx32 "\n", uc.GetPrimType());
}

// void uc_check(const UserConfig& uc)
//{
//	EXIT_NOT_IMPLEMENTED(uc.GetPrimType() != 4);
//}

static void rt_print(const char* func, const RenderTarget& rt)
{
	printf("%s\n", func);

	printf("\t base_addr                           = 0x%016" PRIx64 "\n", rt.base_addr);
	printf("\t pitch_div8_minus1                   = 0x%08" PRIx32 "\n", rt.pitch_div8_minus1);
	printf("\t fmask_pitch_div8_minus1             = 0x%08" PRIx32 "\n", rt.fmask_pitch_div8_minus1);
	printf("\t slice_div64_minus1                  = 0x%08" PRIx32 "\n", rt.slice_div64_minus1);
	printf("\t base_array_slice_index              = 0x%08" PRIx32 "\n", rt.base_array_slice_index);
	printf("\t last_array_slice_index              = 0x%08" PRIx32 "\n", rt.last_array_slice_index);
	printf("\t color_info.fmask_compression_enable = %s\n", rt.color_info.fmask_compression_enable ? "true" : "false");
	printf("\t color_info.fmask_compression_mode   = 0x%08" PRIx32 "\n", rt.color_info.fmask_compression_mode);
	printf("\t color_info.cmask_fast_clear_enable  = %s\n", rt.color_info.cmask_fast_clear_enable ? "true" : "false");
	printf("\t color_info.dcc_compression_enable   = %s\n", rt.color_info.dcc_compression_enable ? "true" : "false");
	printf("\t color_info.neo_mode                 = %s\n", rt.color_info.neo_mode ? "true" : "false");
	printf("\t color_info.cmask_tile_mode          = 0x%08" PRIx32 "\n", rt.color_info.cmask_tile_mode);
	printf("\t color_info.cmask_tile_mode_neo      = 0x%08" PRIx32 "\n", rt.color_info.cmask_tile_mode_neo);
	printf("\t color_info.format                   = 0x%08" PRIx32 "\n", rt.color_info.format);
	printf("\t color_info.channel_type             = 0x%08" PRIx32 "\n", rt.color_info.channel_type);
	printf("\t color_info.channel_order            = 0x%08" PRIx32 "\n", rt.color_info.channel_order);
	printf("\t force_dest_alpha_to_one             = %s\n", rt.force_dest_alpha_to_one ? "true" : "false");
	printf("\t tile_mode                           = 0x%08" PRIx32 "\n", rt.tile_mode);
	printf("\t fmask_tile_mode                     = 0x%08" PRIx32 "\n", rt.fmask_tile_mode);
	printf("\t num_samples                         = 0x%08" PRIx32 "\n", rt.num_samples);
	printf("\t num_fragments                       = 0x%08" PRIx32 "\n", rt.num_fragments);
	printf("\t dcc_max_uncompressed_block_size     = 0x%08" PRIx32 "\n", rt.dcc_max_uncompressed_block_size);
	printf("\t dcc_max_compressed_block_size       = 0x%08" PRIx32 "\n", rt.dcc_max_compressed_block_size);
	printf("\t dcc_min_compressed_block_size       = 0x%08" PRIx32 "\n", rt.dcc_min_compressed_block_size);
	printf("\t dcc_color_transform                 = 0x%08" PRIx32 "\n", rt.dcc_color_transform);
	printf("\t dcc_enable_overwrite_combiner       = %s\n", rt.dcc_enable_overwrite_combiner ? "true" : "false");
	printf("\t dcc_force_independent_blocks        = %s\n", rt.dcc_force_independent_blocks ? "true" : "false");
	printf("\t cmask_addr                          = 0x%016" PRIx64 "\n", rt.cmask_addr);
	printf("\t cmask_slice_minus1                  = 0x%08" PRIx32 "\n", rt.cmask_slice_minus1);
	printf("\t fmask_addr                          = 0x%016" PRIx64 "\n", rt.fmask_addr);
	printf("\t fmask_slice_minus1                  = 0x%08" PRIx32 "\n", rt.fmask_slice_minus1);
	printf("\t clear_color_word0                   = 0x%08" PRIx32 "\n", rt.clear_color_word0);
	printf("\t clear_color_word1                   = 0x%08" PRIx32 "\n", rt.clear_color_word1);
	printf("\t dcc_addr                            = 0x%016" PRIx64 "\n", rt.dcc_addr);
	printf("\t width                               = 0x%08" PRIx32 "\n", rt.width);
	printf("\t height                              = 0x%08" PRIx32 "\n", rt.height);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void rt_check(const RenderTarget& rt)
{
	if (rt.base_addr != 0)
	{
		bool render_to_texture = (rt.tile_mode == 0x0d);
		// EXIT_NOT_IMPLEMENTED(rt.base_addr == 0);
		// EXIT_NOT_IMPLEMENTED(rt.pitch_div8_minus1 != 0x000000ef);
		// EXIT_NOT_IMPLEMENTED(rt.fmask_pitch_div8_minus1 != 0x000000ef);
		// EXIT_NOT_IMPLEMENTED(rt.slice_div64_minus1 != 0x000086ff);
		EXIT_NOT_IMPLEMENTED(rt.base_array_slice_index != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.last_array_slice_index != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.color_info.fmask_compression_enable != false);
		EXIT_NOT_IMPLEMENTED(rt.color_info.fmask_compression_mode != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.color_info.cmask_fast_clear_enable != false);
		EXIT_NOT_IMPLEMENTED(rt.color_info.dcc_compression_enable != false);
		EXIT_NOT_IMPLEMENTED(!render_to_texture && rt.color_info.neo_mode != Config::IsNeo());
		EXIT_NOT_IMPLEMENTED(rt.color_info.cmask_tile_mode != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.color_info.cmask_tile_mode_neo != 0x00000000);
		//		 EXIT_NOT_IMPLEMENTED(rt.format != 0x0000000a);
		// EXIT_NOT_IMPLEMENTED(rt.channel_type != 0x00000006);
		// EXIT_NOT_IMPLEMENTED(rt.channel_order != 0x00000001);
		EXIT_NOT_IMPLEMENTED(rt.force_dest_alpha_to_one != false);
		// EXIT_NOT_IMPLEMENTED(rt.tile_mode != 0x0000000a);
		// EXIT_NOT_IMPLEMENTED(rt.fmask_tile_mode != 0x0000000a);
		EXIT_NOT_IMPLEMENTED(rt.num_samples != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.num_fragments != 0x00000000);
		// EXIT_NOT_IMPLEMENTED(rt.dcc_max_uncompressed_block_size != 0x00000002);
		EXIT_NOT_IMPLEMENTED(rt.dcc_max_compressed_block_size != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.dcc_min_compressed_block_size != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.dcc_color_transform != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.dcc_enable_overwrite_combiner != false);
		EXIT_NOT_IMPLEMENTED(rt.dcc_force_independent_blocks != false);
		EXIT_NOT_IMPLEMENTED(rt.cmask_addr != 0x0000000000000000);
		EXIT_NOT_IMPLEMENTED(rt.cmask_slice_minus1 != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.fmask_addr != 0x0000000000000000);
		EXIT_NOT_IMPLEMENTED(rt.fmask_slice_minus1 != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.clear_color_word0 != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.clear_color_word1 != 0x00000000);
		EXIT_NOT_IMPLEMENTED(rt.dcc_addr != 0x0000000000000000);
		// EXIT_NOT_IMPLEMENTED(rt.width != 0x00000780);
		// EXIT_NOT_IMPLEMENTED(rt.height != 0x00000438);
	}
}

static void z_print(const char* func, const DepthRenderTarget& z)
{
	printf("%s\n", func);

	printf("\t z_info.format                         = 0x%08" PRIx32 "\n", z.z_info.format);
	printf("\t z_info.tile_mode_index                = 0x%08" PRIx32 "\n", z.z_info.tile_mode_index);
	printf("\t z_info.num_samples                    = 0x%08" PRIx32 "\n", z.z_info.num_samples);
	printf("\t z_info.tile_surface_enable            = %s\n", z.z_info.tile_surface_enable ? "true" : "false");
	printf("\t z_info.expclear_enabled               = %s\n", z.z_info.expclear_enabled ? "true" : "false");
	printf("\t z_info.zrange_precision               = 0x%08" PRIx32 "\n", z.z_info.zrange_precision);
	printf("\t stencil_info.format                   = 0x%08" PRIx32 "\n", z.stencil_info.format);
	printf("\t stencil_info.tile_stencil_disable     = %s\n", z.stencil_info.tile_stencil_disable ? "true" : "false");
	printf("\t stencil_info.expclear_enabled         = %s\n", z.stencil_info.expclear_enabled ? "true" : "false");
	printf("\t stencil_info.tile_mode_index          = 0x%08" PRIx32 "\n", z.stencil_info.tile_mode_index);
	printf("\t stencil_info.tile_split               = 0x%08" PRIx32 "\n", z.stencil_info.tile_split);
	printf("\t depth_info.addr5_swizzle_mask         = 0x%08" PRIx32 "\n", z.depth_info.addr5_swizzle_mask);
	printf("\t depth_info.array_mode                 = 0x%08" PRIx32 "\n", z.depth_info.array_mode);
	printf("\t depth_info.pipe_config                = 0x%08" PRIx32 "\n", z.depth_info.pipe_config);
	printf("\t depth_info.bank_width                 = 0x%08" PRIx32 "\n", z.depth_info.bank_width);
	printf("\t depth_info.bank_height                = 0x%08" PRIx32 "\n", z.depth_info.bank_height);
	printf("\t depth_info.macro_tile_aspect          = 0x%08" PRIx32 "\n", z.depth_info.macro_tile_aspect);
	printf("\t depth_info.num_banks                  = 0x%08" PRIx32 "\n", z.depth_info.num_banks);
	printf("\t depth_view.slice_start                = 0x%08" PRIx32 "\n", z.depth_view.slice_start);
	printf("\t depth_view.slice_max                  = 0x%08" PRIx32 "\n", z.depth_view.slice_max);
	printf("\t htile_surface.linear                  = 0x%08" PRIx32 "\n", z.htile_surface.linear);
	printf("\t htile_surface.full_cache              = 0x%08" PRIx32 "\n", z.htile_surface.full_cache);
	printf("\t htile_surface.htile_uses_preload_win  = 0x%08" PRIx32 "\n", z.htile_surface.htile_uses_preload_win);
	printf("\t htile_surface.preload                 = 0x%08" PRIx32 "\n", z.htile_surface.preload);
	printf("\t htile_surface.prefetch_width          = 0x%08" PRIx32 "\n", z.htile_surface.prefetch_width);
	printf("\t htile_surface.prefetch_height         = 0x%08" PRIx32 "\n", z.htile_surface.prefetch_height);
	printf("\t htile_surface.dst_outside_zero_to_one = 0x%08" PRIx32 "\n", z.htile_surface.dst_outside_zero_to_one);
	printf("\t z_read_base_addr                      = 0x%016" PRIx64 "\n", z.z_read_base_addr);
	printf("\t stencil_read_base_addr                = 0x%016" PRIx64 "\n", z.stencil_read_base_addr);
	printf("\t z_write_base_addr                     = 0x%016" PRIx64 "\n", z.z_write_base_addr);
	printf("\t stencil_write_base_addr               = 0x%016" PRIx64 "\n", z.stencil_write_base_addr);
	printf("\t pitch_div8_minus1                     = 0x%08" PRIx32 "\n", z.pitch_div8_minus1);
	printf("\t height_div8_minus1                    = 0x%08" PRIx32 "\n", z.height_div8_minus1);
	printf("\t slice_div64_minus1                    = 0x%08" PRIx32 "\n", z.slice_div64_minus1);
	printf("\t htile_data_base_addr                  = 0x%016" PRIx64 "\n", z.htile_data_base_addr);
	printf("\t width                                 = 0x%08" PRIx32 "\n", z.width);
	printf("\t height                                = 0x%08" PRIx32 "\n", z.height);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void z_check(const DepthRenderTarget& z)
{
	if (z.z_info.format == 0)
	{
		EXIT_NOT_IMPLEMENTED(z.z_info.format != 0);
		EXIT_NOT_IMPLEMENTED(z.z_info.tile_mode_index != 0);
		EXIT_NOT_IMPLEMENTED(z.z_info.num_samples != 0);
		EXIT_NOT_IMPLEMENTED(z.z_info.tile_surface_enable != false);
		EXIT_NOT_IMPLEMENTED(z.z_info.expclear_enabled != false);
		EXIT_NOT_IMPLEMENTED(z.z_info.zrange_precision != 0);
	} else
	{
		EXIT_NOT_IMPLEMENTED(z.z_info.format != 0x00000003);
		// EXIT_NOT_IMPLEMENTED(z.z_info.tile_mode_index != (Config::IsNeo() ? 0x00000002 : 0));
		EXIT_NOT_IMPLEMENTED(z.z_info.num_samples != 0x00000000);
		// EXIT_NOT_IMPLEMENTED(z.z_info.tile_surface_enable != true);
		EXIT_NOT_IMPLEMENTED(z.z_info.expclear_enabled != false);
		EXIT_NOT_IMPLEMENTED(z.z_info.zrange_precision != 0x00000001);
	}

	if (z.stencil_info.format == 0)
	{
		EXIT_NOT_IMPLEMENTED(z.stencil_info.format != 0);
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_stencil_disable != false);
		EXIT_NOT_IMPLEMENTED(z.stencil_info.expclear_enabled != false);
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_mode_index != 0);
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_split != 0);
	} else
	{
		EXIT_NOT_IMPLEMENTED(z.stencil_info.format != 0x00000001);
		EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_stencil_disable != true);
		EXIT_NOT_IMPLEMENTED(z.stencil_info.expclear_enabled != false);
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_mode_index != (Config::IsNeo() ? 0x00000002 : 0));
		// EXIT_NOT_IMPLEMENTED(z.stencil_info.tile_split != (Config::IsNeo() ? 0x00000002 : 0));
	}

	if (z.z_info.format != 0 || z.stencil_info.format != 0)
	{
		EXIT_NOT_IMPLEMENTED(z.depth_info.addr5_swizzle_mask != 0x00000001);
		EXIT_NOT_IMPLEMENTED(z.depth_info.array_mode != 0x00000004);
		EXIT_NOT_IMPLEMENTED(z.depth_info.pipe_config != (Config::IsNeo() ? 0x00000012 : 0x0c));
		EXIT_NOT_IMPLEMENTED(z.depth_info.bank_width != 0x00000000);
		// EXIT_NOT_IMPLEMENTED(z.depth_info.bank_height != (Config::IsNeo() ? 0x00000001 : 2));
		// EXIT_NOT_IMPLEMENTED(z.depth_info.macro_tile_aspect != (Config::IsNeo() ? 0x00000000 : 2));
		// EXIT_NOT_IMPLEMENTED(z.depth_info.num_banks != (Config::IsNeo() ? 0x00000002 : 3));
		EXIT_NOT_IMPLEMENTED(z.depth_view.slice_start != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.depth_view.slice_max != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.linear != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.full_cache != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.htile_uses_preload_win != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.preload != 0x00000001);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.prefetch_width != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.prefetch_height != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.htile_surface.dst_outside_zero_to_one != 0x00000000);
		EXIT_NOT_IMPLEMENTED(z.z_read_base_addr != z.z_write_base_addr);
		EXIT_NOT_IMPLEMENTED(z.stencil_read_base_addr != z.stencil_write_base_addr);
		EXIT_NOT_IMPLEMENTED(z.z_write_base_addr == 0);
		EXIT_NOT_IMPLEMENTED(z.stencil_info.format != 0 && z.stencil_write_base_addr == 0);
		// EXIT_NOT_IMPLEMENTED(z.pitch_div8_minus1 != 0x000000ff);
		// EXIT_NOT_IMPLEMENTED(z.height_div8_minus1 != 0x0000008f);
		// EXIT_NOT_IMPLEMENTED(z.slice_div64_minus1 != 0x00008fff);
		// EXIT_NOT_IMPLEMENTED(z.htile_data_base_addr == 0);
		// EXIT_NOT_IMPLEMENTED(z.width != 0x00000780);
		// EXIT_NOT_IMPLEMENTED(z.height != 0x00000438);
	}
}

static void clip_print(const char* func, const ClipControl& c)
{
	printf("%s\n", func);

	printf("\t user_clip_planes                    = 0x%08" PRIx32 "\n", c.user_clip_planes);
	printf("\t user_clip_plane_mode                = 0x%08" PRIx32 "\n", c.user_clip_plane_mode);
	printf("\t clip_space                          = 0x%08" PRIx32 "\n", c.clip_space);
	printf("\t vertex_kill_mode                    = 0x%08" PRIx32 "\n", c.vertex_kill_mode);
	printf("\t min_z_clip_enable                   = 0x%08" PRIx32 "\n", c.min_z_clip_enable);
	printf("\t max_z_clip_enable                   = 0x%08" PRIx32 "\n", c.max_z_clip_enable);
	printf("\t user_clip_plane_negate_y            = %s\n", c.user_clip_plane_negate_y ? "true" : "false");
	printf("\t clip_enable                         = %s\n", c.clip_enable ? "true" : "false");
	printf("\t user_clip_plane_cull_only           = %s\n", c.user_clip_plane_cull_only ? "true" : "false");
	printf("\t cull_on_clipping_error_disable      = %s\n", c.cull_on_clipping_error_disable ? "true" : "false");
	printf("\t linear_attribute_clip_enable        = %s\n", c.linear_attribute_clip_enable ? "true" : "false");
	printf("\t force_viewport_index_from_vs_enable = %s\n", c.force_viewport_index_from_vs_enable ? "true" : "false");
}

static void clip_check(const ClipControl& c)
{
	EXIT_NOT_IMPLEMENTED(c.user_clip_planes != 0);
	EXIT_NOT_IMPLEMENTED(c.user_clip_plane_mode != 0);
	EXIT_NOT_IMPLEMENTED(c.clip_space != 0);
	EXIT_NOT_IMPLEMENTED(c.vertex_kill_mode != 0);
	EXIT_NOT_IMPLEMENTED(c.min_z_clip_enable != 0);
	EXIT_NOT_IMPLEMENTED(c.max_z_clip_enable != 0);
	EXIT_NOT_IMPLEMENTED(c.user_clip_plane_negate_y != false);
	EXIT_NOT_IMPLEMENTED(c.clip_enable != false);
	EXIT_NOT_IMPLEMENTED(c.user_clip_plane_cull_only != false);
	EXIT_NOT_IMPLEMENTED(c.cull_on_clipping_error_disable != false);
	EXIT_NOT_IMPLEMENTED(c.linear_attribute_clip_enable != false);
	EXIT_NOT_IMPLEMENTED(c.force_viewport_index_from_vs_enable != false);
}

static void rc_print(const char* func, const RenderControl& c)
{
	printf("%s\n", func);

	printf("\t depth_clear_enable       = %s\n", c.depth_clear_enable ? "true" : "false");
	printf("\t stencil_clear_enable     = %s\n", c.stencil_clear_enable ? "true" : "false");
	printf("\t resummarize_enable       = %s\n", c.resummarize_enable ? "true" : "false");
	printf("\t stencil_compress_disable = %s\n", c.stencil_compress_disable ? "true" : "false");
	printf("\t depth_compress_disable   = %s\n", c.depth_compress_disable ? "true" : "false");
	printf("\t copy_centroid            = %s\n", c.copy_centroid ? "true" : "false");
	printf("\t copy_sample              = %" PRIu8 "\n", c.copy_sample);
}

static void rc_check(const RenderControl& c)
{
	// EXIT_NOT_IMPLEMENTED(c.depth_clear_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.stencil_clear_enable != false);
	EXIT_NOT_IMPLEMENTED(c.resummarize_enable != false);
	EXIT_NOT_IMPLEMENTED(c.stencil_compress_disable != false);
	EXIT_NOT_IMPLEMENTED(c.depth_compress_disable != false);
	EXIT_NOT_IMPLEMENTED(c.copy_centroid != false);
	EXIT_NOT_IMPLEMENTED(c.copy_sample != 0);
}

static void mc_print(const char* func, const ModeControl& c)
{
	printf("%s\n", func);

	printf("\t cull_front               = %s\n", c.cull_front ? "true" : "false");
	printf("\t cull_back                = %s\n", c.cull_back ? "true" : "false");
	printf("\t face                     = %s\n", c.face ? "true" : "false");
	printf("\t poly_mode                = %" PRIu8 "\n", c.poly_mode);
	printf("\t polymode_front_ptype     = %" PRIu8 "\n", c.polymode_front_ptype);
	printf("\t polymode_back_ptype      = %" PRIu8 "\n", c.polymode_back_ptype);
	printf("\t poly_offset_front_enable = %s\n", c.poly_offset_front_enable ? "true" : "false");
	printf("\t poly_offset_back_enable  = %s\n", c.poly_offset_back_enable ? "true" : "false");
	printf("\t vtx_window_offset_enable = %s\n", c.vtx_window_offset_enable ? "true" : "false");
	printf("\t provoking_vtx_last       = %s\n", c.provoking_vtx_last ? "true" : "false");
	printf("\t persp_corr_dis           = %s\n", c.persp_corr_dis ? "true" : "false");
}

static void mc_check(const ModeControl& c)
{
	EXIT_NOT_IMPLEMENTED(c.cull_front != false);
	// EXIT_NOT_IMPLEMENTED(c.cull_back != false);
	EXIT_NOT_IMPLEMENTED(c.face != false);
	EXIT_NOT_IMPLEMENTED(c.poly_mode != 0);
	EXIT_NOT_IMPLEMENTED(c.polymode_front_ptype != 0 && c.polymode_front_ptype != 2);
	EXIT_NOT_IMPLEMENTED(c.polymode_back_ptype != 0 && c.polymode_back_ptype != 2);
	EXIT_NOT_IMPLEMENTED(c.poly_offset_front_enable != false);
	EXIT_NOT_IMPLEMENTED(c.poly_offset_back_enable != false);
	EXIT_NOT_IMPLEMENTED(c.vtx_window_offset_enable != false);
	EXIT_NOT_IMPLEMENTED(c.provoking_vtx_last != false);
	EXIT_NOT_IMPLEMENTED(c.persp_corr_dis != false);
}

static void bc_print(const char* func, const BlendControl& c, const BlendColor& color)
{
	printf("%s\n", func);

	printf("\t color_srcblend       = %" PRIu8 "\n", c.color_srcblend);
	printf("\t color_comb_fcn       = %" PRIu8 "\n", c.color_comb_fcn);
	printf("\t color_destblend      = %" PRIu8 "\n", c.color_destblend);
	printf("\t alpha_srcblend       = %" PRIu8 "\n", c.alpha_srcblend);
	printf("\t alpha_comb_fcn       = %" PRIu8 "\n", c.alpha_comb_fcn);
	printf("\t alpha_destblend      = %" PRIu8 "\n", c.alpha_destblend);
	printf("\t separate_alpha_blend = %s\n", c.separate_alpha_blend ? "true" : "false");
	printf("\t enable               = %s\n", c.enable ? "true" : "false");
	printf("\t red                  = %f\n", color.red);
	printf("\t green                = %f\n", color.green);
	printf("\t blue                 = %f\n", color.blue);
	printf("\t alpha                = %f\n", color.alpha);
}

static void bc_check(const BlendControl& c, const BlendColor& color)
{
	// EXIT_NOT_IMPLEMENTED(c.color_srcblend != 0);
	EXIT_NOT_IMPLEMENTED(c.color_comb_fcn != 0);
	// EXIT_NOT_IMPLEMENTED(c.color_destblend != 0);
	EXIT_NOT_IMPLEMENTED(c.alpha_srcblend != 0);
	EXIT_NOT_IMPLEMENTED(c.alpha_comb_fcn != 0);
	EXIT_NOT_IMPLEMENTED(c.alpha_destblend != 0);
	EXIT_NOT_IMPLEMENTED(c.separate_alpha_blend != false);
	// EXIT_NOT_IMPLEMENTED(c.enable != false);
	EXIT_NOT_IMPLEMENTED(color.red != 0.0f);
	EXIT_NOT_IMPLEMENTED(color.green != 0.0f);
	EXIT_NOT_IMPLEMENTED(color.blue != 0.0f);
	EXIT_NOT_IMPLEMENTED(color.alpha != 0.0f);
}

static void d_print(const char* func, const DepthControl& c, const StencilControl& s, const StencilMask& sm)
{
	printf("%s\n", func);

	printf("\t stencil_enable       = %s\n", c.stencil_enable ? "true" : "false");
	printf("\t z_enable             = %s\n", c.z_enable ? "true" : "false");
	printf("\t z_write_enable       = %s\n", c.z_write_enable ? "true" : "false");
	printf("\t depth_bounds_enable  = %s\n", c.depth_bounds_enable ? "true" : "false");
	printf("\t zfunc                = %" PRIu8 "\n", c.zfunc);
	printf("\t backface_enable      = %s\n", c.backface_enable ? "true" : "false");
	printf("\t stencilfunc          = %" PRIu8 "\n", c.stencilfunc);
	printf("\t stencilfunc_bf       = %" PRIu8 "\n", c.stencilfunc_bf);
	printf("\t stencil_fail         = %" PRIu8 "\n", s.stencil_fail);
	printf("\t stencil_zpass        = %" PRIu8 "\n", s.stencil_zpass);
	printf("\t stencil_zfail        = %" PRIu8 "\n", s.stencil_zfail);
	printf("\t stencil_fail_bf      = %" PRIu8 "\n", s.stencil_fail_bf);
	printf("\t stencil_zpass_bf     = %" PRIu8 "\n", s.stencil_zpass_bf);
	printf("\t stencil_zfail_bf     = %" PRIu8 "\n", s.stencil_zfail_bf);
	printf("\t stencil_testval      = %" PRIu8 "\n", sm.stencil_testval);
	printf("\t stencil_mask         = %" PRIu8 "\n", sm.stencil_mask);
	printf("\t stencil_writemask    = %" PRIu8 "\n", sm.stencil_writemask);
	printf("\t stencil_opval        = %" PRIu8 "\n", sm.stencil_opval);
	printf("\t stencil_testval_bf   = %" PRIu8 "\n", sm.stencil_testval_bf);
	printf("\t stencil_mask_bf      = %" PRIu8 "\n", sm.stencil_mask_bf);
	printf("\t stencil_writemask_bf = %" PRIu8 "\n", sm.stencil_writemask_bf);
	printf("\t stencil_opval_bf     = %" PRIu8 "\n", sm.stencil_opval_bf);
}

static void d_check(const DepthControl& c, const StencilControl& s, const StencilMask& /*sm*/)
{
	// EXIT_NOT_IMPLEMENTED(c.stencil_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.z_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.z_write_enable != false);
	EXIT_NOT_IMPLEMENTED(c.depth_bounds_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.zfunc != 0);
	EXIT_NOT_IMPLEMENTED(c.backface_enable != false);
	// EXIT_NOT_IMPLEMENTED(c.stencilfunc != 0);
	// EXIT_NOT_IMPLEMENTED(c.stencilfunc_bf != 0);
	// EXIT_NOT_IMPLEMENTED(s.stencil_fail != 0);
	// EXIT_NOT_IMPLEMENTED(s.stencil_zpass != 0);
	// EXIT_NOT_IMPLEMENTED(s.stencil_zfail != 0);
	EXIT_NOT_IMPLEMENTED(s.stencil_fail_bf != 0);
	EXIT_NOT_IMPLEMENTED(s.stencil_zpass_bf != 0);
	EXIT_NOT_IMPLEMENTED(s.stencil_zfail_bf != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_testval != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_mask != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_writemask != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_opval != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_testval_bf != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_mask_bf != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_writemask_bf != 0);
	// EXIT_NOT_IMPLEMENTED(sm.stencil_opval_bf != 0);
}

static void eqaa_print(const char* func, const EqaaControl& c)
{
	printf("%s\n", func);

	printf("\t max_anchor_samples         = %" PRIu8 "\n", c.max_anchor_samples);
	printf("\t ps_iter_samples            = %" PRIu8 "\n", c.ps_iter_samples);
	printf("\t mask_export_num_samples    = %" PRIu8 "\n", c.mask_export_num_samples);
	printf("\t alpha_to_mask_num_samples  = %" PRIu8 "\n", c.alpha_to_mask_num_samples);
	printf("\t high_quality_intersections = %s\n", c.high_quality_intersections ? "true" : "false");
	printf("\t incoherent_eqaa_reads      = %s\n", c.incoherent_eqaa_reads ? "true" : "false");
	printf("\t interpolate_comp_z         = %s\n", c.interpolate_comp_z ? "true" : "false");
	printf("\t static_anchor_associations = %s\n", c.static_anchor_associations ? "true" : "false");
}

static void eqaa_check(const EqaaControl& c)
{
	EXIT_NOT_IMPLEMENTED(c.max_anchor_samples != 0);
	EXIT_NOT_IMPLEMENTED(c.ps_iter_samples != 0);
	EXIT_NOT_IMPLEMENTED(c.mask_export_num_samples != 0);
	EXIT_NOT_IMPLEMENTED(c.alpha_to_mask_num_samples != 0);
	EXIT_NOT_IMPLEMENTED(c.high_quality_intersections != false);
	EXIT_NOT_IMPLEMENTED(c.incoherent_eqaa_reads != false);
	EXIT_NOT_IMPLEMENTED(c.interpolate_comp_z != false);
	EXIT_NOT_IMPLEMENTED(c.static_anchor_associations != false);
}

static void vp_print(const char* func, const ScreenViewport& vp)
{
	printf("%s\n", func);

	printf("\t viewports[0].zmin       = %f\n", vp.viewports[0].zmin);
	printf("\t viewports[0].zmax       = %f\n", vp.viewports[0].zmax);
	printf("\t viewports[0].xscale     = %f\n", vp.viewports[0].xscale);
	printf("\t viewports[0].xoffset    = %f\n", vp.viewports[0].xoffset);
	printf("\t viewports[0].yscale     = %f\n", vp.viewports[0].yscale);
	printf("\t viewports[0].yoffset    = %f\n", vp.viewports[0].yoffset);
	printf("\t viewports[0].zscale     = %f\n", vp.viewports[0].zscale);
	printf("\t viewports[0].zoffset    = %f\n", vp.viewports[0].zoffset);
	printf("\t transform_control       = 0x%08" PRIx32 "\n", vp.transform_control);
	printf("\t scissor_left            = %d\n", vp.scissor_left);
	printf("\t scissor_top             = %d\n", vp.scissor_top);
	printf("\t scissor_right           = %d\n", vp.scissor_right);
	printf("\t scissor_bottom          = %d\n", vp.scissor_bottom);
	printf("\t hw_offset_x             = %u\n", vp.hw_offset_x);
	printf("\t hw_offset_y             = %u\n", vp.hw_offset_y);
	printf("\t guard_band_horz_clip    = %f\n", vp.guard_band_horz_clip);
	printf("\t guard_band_vert_clip    = %f\n", vp.guard_band_vert_clip);
	printf("\t guard_band_horz_discard = %f\n", vp.guard_band_horz_discard);
	printf("\t guard_band_vert_discard = %f\n", vp.guard_band_vert_discard);
}

static void vp_check(const ScreenViewport& vp)
{
	EXIT_NOT_IMPLEMENTED(vp.viewports[0].zmin != 0.000000);
	EXIT_NOT_IMPLEMENTED(vp.viewports[0].zmax != 1.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].xscale != 960.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].xoffset != 960.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].yscale != -540.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].yoffset != 540.000000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].zscale != 0.500000);
	// EXIT_NOT_IMPLEMENTED(vp.viewports[0].zoffset != 0.500000);
	EXIT_NOT_IMPLEMENTED(vp.transform_control != 1087);
	// EXIT_NOT_IMPLEMENTED(vp.scissor_left != 0);
	// EXIT_NOT_IMPLEMENTED(vp.scissor_top != 0);
	// EXIT_NOT_IMPLEMENTED(vp.scissor_right != 1920);
	// EXIT_NOT_IMPLEMENTED(vp.scissor_bottom != 1080);
	// EXIT_NOT_IMPLEMENTED(vp.hw_offset_x != 60);
	// EXIT_NOT_IMPLEMENTED(vp.hw_offset_y != 32);
	// EXIT_NOT_IMPLEMENTED(fabsf(vp.guard_band_horz_clip - 33.133327f) > 0.001f);
	// EXIT_NOT_IMPLEMENTED(fabsf(vp.guard_band_vert_clip - 59.629623f) > 0.001f);
	EXIT_NOT_IMPLEMENTED(vp.guard_band_horz_discard != 1.000000);
	EXIT_NOT_IMPLEMENTED(vp.guard_band_vert_discard != 1.000000);
}

static void hw_check(const HardwareContext& hw)
{
	const auto& rt   = hw.GetRenderTargets(0);
	const auto& bc   = hw.GetBlendControl(0);
	const auto& bclr = hw.GetBlendColor();
	const auto& vp   = hw.GetScreenViewport();
	const auto& z    = hw.GetDepthRenderTarget();
	const auto& c    = hw.GetClipControl();
	const auto& rc   = hw.GetRenderControl();
	const auto& d    = hw.GetDepthControl();
	const auto& s    = hw.GetStencilControl();
	const auto& sm   = hw.GetStencilMask();
	const auto& mc   = hw.GetModeControl();
	const auto& eqaa = hw.GetEqaaControl();

	rt_check(rt);
	vp_check(vp);
	z_check(z);
	clip_check(c);
	rc_check(rc);
	d_check(d, s, sm);
	mc_check(mc);
	bc_check(bc, bclr);
	eqaa_check(eqaa);

	EXIT_NOT_IMPLEMENTED(hw.GetRenderTargetMask() != 0xF && hw.GetRenderTargetMask() != 0x0);
	EXIT_NOT_IMPLEMENTED(hw.GetDepthClearValue() != 0.0f && hw.GetDepthClearValue() != 1.0f);
	// EXIT_NOT_IMPLEMENTED(hw.GetStencilClearValue() != 0);
}

static void hw_print(const HardwareContext& hw)
{
	const auto& rt   = hw.GetRenderTargets(0);
	const auto& bc   = hw.GetBlendControl(0);
	const auto& bclr = hw.GetBlendColor();
	const auto& vp   = hw.GetScreenViewport();
	const auto& z    = hw.GetDepthRenderTarget();
	const auto& c    = hw.GetClipControl();
	const auto& rc   = hw.GetRenderControl();
	const auto& d    = hw.GetDepthControl();
	const auto& s    = hw.GetStencilControl();
	const auto& sm   = hw.GetStencilMask();
	const auto& mc   = hw.GetModeControl();
	const auto& eqaa = hw.GetEqaaControl();

	printf("HardwareContext\n");
	printf("\t GetRenderTargetMask() = 0x%08" PRIx32 "\n", hw.GetRenderTargetMask());
	printf("\t GetDepthClearValue()  = %f\n", hw.GetDepthClearValue());
	printf("\t GetStencilClearValue()  = %" PRIu8 "\n", hw.GetStencilClearValue());

	rt_print("RenderTraget:", rt);
	z_print("DepthRenderTraget:", z);
	vp_print("ScreenViewport:", vp);
	clip_print("ClipControl:", c);
	rc_print("RenderControl:", rc);
	d_print("DepthStencilControlMask:", d, s, sm);
	mc_print("ModeControl:", mc);
	bc_print("BlendControl:", bc, bclr);
	eqaa_print("EqaaControl:", eqaa);
}

void GraphicsRenderInit()
{
	EXIT_IF(g_render_ctx != nullptr);

	g_render_ctx = new RenderContext;
}

void GraphicsRenderCreateContext()
{
	EXIT_IF(g_render_ctx == nullptr);

	g_render_ctx->SetGraphicCtx(WindowGetGraphicContext());
}

void RenderContext::AddEopEq(LibKernel::EventQueue::KernelEqueue eq)
{
	Core::LockGuard lock(m_eop_mutex);

	EXIT_NOT_IMPLEMENTED(m_eop_eqs.Contains(eq));

	m_eop_eqs.Add(eq);
}

void RenderContext::DeleteEopEq(LibKernel::EventQueue::KernelEqueue eq)
{
	Core::LockGuard lock(m_eop_mutex);

	auto index = m_eop_eqs.Find(eq);
	EXIT_NOT_IMPLEMENTED(!m_eop_eqs.IndexValid(index));
	m_eop_eqs[index] = nullptr;
}

void RenderContext::TriggerEopEvent()
{
	Core::LockGuard lock(m_eop_mutex);

	for (auto& eop_eq: m_eop_eqs)
	{
		if (eop_eq != nullptr)
		{
			auto result =
			    LibKernel::EventQueue::KernelTriggerEvent(eop_eq, GRAPHICS_EVENT_EOP, LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS,
			                                              reinterpret_cast<void*>(LibKernel::KernelReadTsc()));
			EXIT_NOT_IMPLEMENTED(result != OK);
		}
	}
}

void GdsBuffer::Init(GraphicContext* ctx)
{
	if (m_buffer == nullptr)
	{
		m_buffer = new VulkanBuffer;

		m_buffer->usage           = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		m_buffer->memory.property = static_cast<uint32_t>(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
		                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
		m_buffer->buffer = nullptr;

		VulkanCreateBuffer(ctx, DW_SIZE * 4, m_buffer);
		EXIT_NOT_IMPLEMENTED(m_buffer->buffer == nullptr);
	}
}

void GdsBuffer::Clear(GraphicContext* ctx, uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value)
{
	EXIT_IF(ctx == nullptr);

	Core::LockGuard lock(m_mutex);

	Init(ctx);

	EXIT_NOT_IMPLEMENTED(dw_offset >= DW_SIZE);
	EXIT_NOT_IMPLEMENTED(dw_offset + dw_num > DW_SIZE);

	EXIT_IF(m_buffer == nullptr);

	void* data = nullptr;
	VulkanMapMemory(ctx, &m_buffer->memory, &data);

	EXIT_IF(data == nullptr);

	for (uint32_t i = 0; i < dw_num; i++)
	{
		static_cast<uint32_t*>(data)[dw_offset + i] = clear_value;
	}

	VulkanUnmapMemory(ctx, &m_buffer->memory);
}

void GdsBuffer::Read(GraphicContext* ctx, uint32_t* dst, uint32_t dw_offset, uint32_t dw_size)
{
	EXIT_IF(dst == nullptr);

	Core::LockGuard lock(m_mutex);

	Init(ctx);

	EXIT_NOT_IMPLEMENTED(dw_offset >= DW_SIZE);
	EXIT_NOT_IMPLEMENTED(dw_offset + dw_size > DW_SIZE);

	EXIT_IF(m_buffer == nullptr);

	void* data = nullptr;
	VulkanMapMemory(ctx, &m_buffer->memory, &data);

	EXIT_IF(data == nullptr);

	for (uint32_t i = 0; i < dw_size; i++)
	{
		dst[i] = static_cast<uint32_t*>(data)[dw_offset + i];
	}

	VulkanUnmapMemory(ctx, &m_buffer->memory);
}

VulkanBuffer* GdsBuffer::GetBuffer(GraphicContext* ctx)
{
	Core::LockGuard lock(m_mutex);

	Init(ctx);

	return m_buffer;
}

void CommandPool::Create()
{
	auto* ctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(m_pool != nullptr);

	m_pool = new VulkanCommandPool;

	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->device == nullptr);
	EXIT_IF(ctx->queue_family_index == static_cast<uint32_t>(-1));
	EXIT_IF(m_pool->pool != nullptr);
	EXIT_IF(m_pool->buffers != nullptr);
	EXIT_IF(m_pool->fences != nullptr);
	EXIT_IF(m_pool->semaphores != nullptr);
	EXIT_IF(m_pool->buffers_count != 0);

	VkCommandPoolCreateInfo pool_info {};
	pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.pNext            = nullptr;
	pool_info.queueFamilyIndex = ctx->queue_family_index;
	pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

	vkCreateCommandPool(ctx->device, &pool_info, nullptr, &m_pool->pool);

	EXIT_NOT_IMPLEMENTED(m_pool->pool == nullptr);

	m_pool->buffers_count = 4;
	m_pool->buffers       = new VkCommandBuffer[m_pool->buffers_count];
	m_pool->fences        = new VkFence[m_pool->buffers_count];
	m_pool->semaphores    = new VkSemaphore[m_pool->buffers_count];
	m_pool->busy          = new bool[m_pool->buffers_count];

	VkCommandBufferAllocateInfo alloc_info {};
	alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc_info.commandPool        = m_pool->pool;
	alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc_info.commandBufferCount = m_pool->buffers_count;

	if (vkAllocateCommandBuffers(ctx->device, &alloc_info, m_pool->buffers) != VK_SUCCESS)
	{
		EXIT("Can't allocate command buffers");
	}

	for (uint32_t i = 0; i < m_pool->buffers_count; i++)
	{
		m_pool->busy[i] = false;

		VkFenceCreateInfo fence_info {};
		fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fence_info.pNext = nullptr;
		fence_info.flags = 0;

		if (vkCreateFence(ctx->device, &fence_info, nullptr, &m_pool->fences[i]) != VK_SUCCESS)
		{
			EXIT("Can't create fence");
		}

		VkSemaphoreCreateInfo semaphore_info {};
		semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		semaphore_info.pNext = nullptr;
		semaphore_info.flags = 0;

		if (vkCreateSemaphore(ctx->device, &semaphore_info, nullptr, &m_pool->semaphores[i]) != VK_SUCCESS)
		{
			EXIT("Can't create semaphore");
		}

		EXIT_IF(m_pool->buffers[i] == nullptr);
		EXIT_IF(m_pool->fences[i] == nullptr);
		EXIT_IF(m_pool->semaphores[i] == nullptr);
	}
}

void CommandPool::Delete()
{
	auto* ctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(m_pool == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->device == nullptr);

	for (uint32_t i = 0; i < m_pool->buffers_count; i++)
	{
		vkDestroySemaphore(ctx->device, m_pool->semaphores[i], nullptr);
		vkDestroyFence(ctx->device, m_pool->fences[i], nullptr);
	}

	vkFreeCommandBuffers(ctx->device, m_pool->pool, m_pool->buffers_count, m_pool->buffers);

	vkDestroyCommandPool(ctx->device, m_pool->pool, nullptr);

	delete[] m_pool->semaphores;
	delete[] m_pool->fences;
	delete[] m_pool->buffers;
	delete[] m_pool->busy;

	delete m_pool;
	m_pool = nullptr;
}

VulkanFramebuffer* FramebufferCache::CreateFramebuffer(RenderColorInfo* color, RenderDepthInfo* depth)
{
	KYTY_PROFILER_FUNCTION();

	static std::atomic<uint64_t> seq = 0;

	Core::LockGuard lock(m_mutex);

	EXIT_IF(color == nullptr);
	EXIT_IF(depth == nullptr);
	EXIT_IF(g_render_ctx == nullptr);

	bool with_depth = (depth->format != VK_FORMAT_UNDEFINED && depth->vulkan_buffer != nullptr);
	bool with_color = (color->vulkan_buffer != nullptr);

	for (auto& f: m_framebuffers)
	{
		if (f.framebuffer != nullptr && f.image_id == (with_color ? color->vulkan_buffer->memory.unique_id : 0) &&
		    f.depth_id == (with_depth ? depth->vulkan_buffer->memory.unique_id : 0) && f.depth_clear_enable == depth->depth_clear_enable &&
		    f.stencil_clear_enable == depth->stencil_clear_enable)
		{
			return f.framebuffer;
		}
	}

	auto* framebuffer        = new VulkanFramebuffer;
	framebuffer->render_pass = nullptr;
	framebuffer->framebuffer = nullptr;

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	EXIT_NOT_IMPLEMENTED(!with_depth && !with_color);
	EXIT_NOT_IMPLEMENTED(with_depth && with_color &&
	                     (color->vulkan_buffer->extent.width != depth->vulkan_buffer->extent.width ||
	                      color->vulkan_buffer->extent.height != depth->vulkan_buffer->extent.height));

	VulkanImage* vulkan_buffer =
	    (with_color ? color->vulkan_buffer
	                : CreateDummyBuffer(VK_FORMAT_B8G8R8A8_SRGB, depth->vulkan_buffer->extent.width, depth->vulkan_buffer->extent.height));

	VkAttachmentDescription attachments[2];
	attachments[0].flags          = 0;
	attachments[0].format         = vulkan_buffer->format;
	attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachments[0].finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	attachments[1].flags          = 0;
	attachments[1].format         = depth->format;
	attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp         = (depth->depth_clear_enable ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
	attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].stencilLoadOp  = (depth->stencil_clear_enable ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[1].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference color_attachment_ref {};
	color_attachment_ref.attachment = (with_color ? 0 : VK_ATTACHMENT_UNUSED);
	color_attachment_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depth_attachment_ref {};
	depth_attachment_ref.attachment = 1;
	depth_attachment_ref.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass {};
	subpass.flags                   = 0;
	subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.inputAttachmentCount    = 0;
	subpass.pInputAttachments       = nullptr;
	subpass.colorAttachmentCount    = 1;
	subpass.pColorAttachments       = &color_attachment_ref;
	subpass.pResolveAttachments     = nullptr;
	subpass.pDepthStencilAttachment = (with_depth ? &depth_attachment_ref : nullptr);
	subpass.preserveAttachmentCount = 0;
	subpass.pPreserveAttachments    = nullptr;

	VkRenderPassCreateInfo render_pass_info {};
	render_pass_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	render_pass_info.pNext           = nullptr;
	render_pass_info.flags           = 0;
	render_pass_info.attachmentCount = (with_depth ? 2 : 1);
	render_pass_info.pAttachments    = attachments;
	render_pass_info.subpassCount    = 1;
	render_pass_info.pSubpasses      = &subpass;
	render_pass_info.dependencyCount = 0;
	render_pass_info.pDependencies   = nullptr;

	vkCreateRenderPass(gctx->device, &render_pass_info, nullptr, &framebuffer->render_pass);

	framebuffer->render_pass_id = ++seq;

	EXIT_NOT_IMPLEMENTED(framebuffer->render_pass == nullptr);

	VkImageView views[] = {vulkan_buffer->image_view, (with_depth ? depth->vulkan_buffer->image_view : nullptr)};

	VkFramebufferCreateInfo framebuffer_info {};
	framebuffer_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer_info.pNext           = nullptr;
	framebuffer_info.flags           = 0;
	framebuffer_info.renderPass      = framebuffer->render_pass;
	framebuffer_info.attachmentCount = with_depth ? 2 : 1;
	framebuffer_info.pAttachments    = views;
	framebuffer_info.width           = vulkan_buffer->extent.width;
	framebuffer_info.height          = vulkan_buffer->extent.height;
	framebuffer_info.layers          = 1;

	vkCreateFramebuffer(gctx->device, &framebuffer_info, nullptr, &framebuffer->framebuffer);

	EXIT_NOT_IMPLEMENTED(framebuffer->framebuffer == nullptr);

	Framebuffer fnew;
	fnew.framebuffer          = framebuffer;
	fnew.image_id             = (with_color ? color->vulkan_buffer->memory.unique_id : 0);
	fnew.depth_id             = (with_depth ? depth->vulkan_buffer->memory.unique_id : 0);
	fnew.depth_clear_enable   = depth->depth_clear_enable;
	fnew.stencil_clear_enable = depth->stencil_clear_enable;

	bool updated = false;

	for (auto& f: m_framebuffers)
	{
		if (f.framebuffer == nullptr)
		{
			f       = fnew;
			updated = true;
			break;
		}
	}

	if (!updated)
	{
		m_framebuffers.Add(fnew);
	}

	return framebuffer;
}

void FramebufferCache::FreeFramebufferByColor(VulkanImage* image)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(image == nullptr);

	Core::LockGuard lock(m_mutex);

	for (auto& f: m_framebuffers)
	{
		if (f.framebuffer != nullptr && f.image_id == image->memory.unique_id)
		{
			g_render_ctx->GetPipelineCache()->DeletePipelines(f.framebuffer);

			auto* gctx = g_render_ctx->GetGraphicCtx();

			EXIT_IF(gctx == nullptr);

			vkDestroyFramebuffer(gctx->device, f.framebuffer->framebuffer, nullptr);

			vkDestroyRenderPass(gctx->device, f.framebuffer->render_pass, nullptr);

			delete f.framebuffer;

			f.framebuffer = nullptr;

			break;
		}
	}
}

void FramebufferCache::FreeFramebufferByDepth(DepthStencilVulkanImage* image)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(image == nullptr);

	Core::LockGuard lock(m_mutex);

	for (auto& f: m_framebuffers)
	{
		if (f.framebuffer != nullptr && f.depth_id == image->memory.unique_id)
		{
			g_render_ctx->GetPipelineCache()->DeletePipelines(f.framebuffer);

			auto* gctx = g_render_ctx->GetGraphicCtx();

			EXIT_IF(gctx == nullptr);

			vkDestroyFramebuffer(gctx->device, f.framebuffer->framebuffer, nullptr);

			vkDestroyRenderPass(gctx->device, f.framebuffer->render_pass, nullptr);

			delete f.framebuffer;

			f.framebuffer = nullptr;

			break;
		}
	}
}

VideoOutVulkanImage* FramebufferCache::CreateDummyBuffer(VkFormat format, uint32_t width, uint32_t height)
{
	for (auto* b: m_dummy_buffers)
	{
		if (b->extent.width == width && b->extent.height == height && b->format == format)
		{
			return b;
		}
	}

	auto* ctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(ctx == nullptr);

	auto* vk_obj = new VideoOutVulkanImage;

	vk_obj->extent.width  = width;
	vk_obj->extent.height = height;
	vk_obj->format        = format;
	vk_obj->image         = nullptr;
	vk_obj->image_view    = nullptr;
	vk_obj->layout        = VK_IMAGE_LAYOUT_UNDEFINED;

	VkImageCreateInfo image_info {};
	image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext         = nullptr;
	image_info.flags         = 0;
	image_info.imageType     = VK_IMAGE_TYPE_2D;
	image_info.extent.width  = vk_obj->extent.width;
	image_info.extent.height = vk_obj->extent.height;
	image_info.extent.depth  = 1;
	image_info.mipLevels     = 1;
	image_info.arrayLayers   = 1;
	image_info.format        = vk_obj->format;
	image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	image_info.initialLayout = vk_obj->layout;
	image_info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	image_info.samples       = VK_SAMPLE_COUNT_1_BIT;

	vkCreateImage(ctx->device, &image_info, nullptr, &vk_obj->image);

	EXIT_NOT_IMPLEMENTED(vk_obj->image == nullptr);

	VulkanMemory mem;

	vkGetImageMemoryRequirements(ctx->device, vk_obj->image, &mem.requirements);

	mem.property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	bool allocated = VulkanAllocate(ctx, &mem);

	EXIT_NOT_IMPLEMENTED(!allocated);

	VulkanBindImageMemory(ctx, vk_obj, &mem);

	vk_obj->memory = mem;

	VkImageViewCreateInfo create_info {};
	create_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create_info.pNext                           = nullptr;
	create_info.flags                           = 0;
	create_info.image                           = vk_obj->image;
	create_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	create_info.format                          = vk_obj->format;
	create_info.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	create_info.subresourceRange.baseArrayLayer = 0;
	create_info.subresourceRange.baseMipLevel   = 0;
	create_info.subresourceRange.layerCount     = 1;
	create_info.subresourceRange.levelCount     = 1;

	vkCreateImageView(ctx->device, &create_info, nullptr, &vk_obj->image_view);

	EXIT_NOT_IMPLEMENTED(vk_obj->image_view == nullptr);

	UtilSetImageLayoutOptimal(vk_obj);

	m_dummy_buffers.Add(vk_obj);

	return vk_obj;
}

VkSampler SamplerCache::GetSampler(uint64_t id)
{
	Core::LockGuard lock(m_mutex);
	if (id < m_samplers.Size())
	{
		return m_samplers.At(id).vk;
	}
	return nullptr;
}

uint64_t SamplerCache::GetSamplerId(const ShaderSamplerResource& r)
{
	Core::LockGuard lock(m_mutex);
	uint32_t        m_samplers_size = m_samplers.Size();
	for (uint32_t i = 0; i < m_samplers_size; i++)
	{
		const auto& s = m_samplers.At(i);
		if (s.r.fields[0] == r.fields[0] && s.r.fields[1] == r.fields[1] && s.r.fields[2] == r.fields[2] && s.r.fields[3] == r.fields[3])
		{
			return i;
		}
	}
	Sampler s;
	s.r  = r;
	s.vk = nullptr;

	bool  aniso       = false;
	float aniso_ratio = 1.0f;
	auto  mag_filter  = r.XyMagFilter();
	auto  min_filter  = r.XyMinFilter();

	if (mag_filter == 2 || mag_filter == 3 || min_filter == 2 || min_filter == 3)
	{
		aniso = true;
		switch (r.MaxAnisoRatio())
		{
			case 0: aniso_ratio = 1.0f; break;
			case 1: aniso_ratio = 2.0f; break;
			case 2: aniso_ratio = 4.0f; break;
			case 3: aniso_ratio = 8.0f; break;
			case 4: aniso_ratio = 16.0f; break;
			default: EXIT("unknown ratio: %d\n", static_cast<int>(r.MaxAnisoRatio()));
		}
	}

	auto  mip_filter = r.MipFilter();
	float min_lod    = 0.0f;
	float max_lod    = 0.0f;
	if (mip_filter != 0)
	{
		min_lod = static_cast<float>(r.MinLod()) / 256.0f;
		max_lod = static_cast<float>(r.MaxLod()) / 256.0f;
	}

	VkSamplerCreateInfo sampler_info {};

	auto get_warp = [](uint8_t clamp)
	{
		switch (clamp)
		{
			case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
			case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
			case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
			case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; break;
			default: EXIT("unknown clamp: %u\n", clamp);
		}
		return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	};

	VkBorderColor border = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
	switch (r.BorderColorType())
	{
		case 0: border = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK; break;
		case 1: border = VK_BORDER_COLOR_INT_OPAQUE_BLACK; break;
		case 2: border = VK_BORDER_COLOR_INT_OPAQUE_WHITE; break;
		default: EXIT("unknown border color: %d", static_cast<int>(r.BorderColorType()));
	}

	sampler_info.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.pNext                   = nullptr;
	sampler_info.flags                   = 0;
	sampler_info.magFilter               = (mag_filter == 0 || mag_filter == 2 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
	sampler_info.minFilter               = (min_filter == 0 || min_filter == 2 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
	sampler_info.mipmapMode              = (mip_filter == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST);
	sampler_info.addressModeU            = get_warp(r.ClampX());
	sampler_info.addressModeV            = get_warp(r.ClampY());
	sampler_info.addressModeW            = get_warp(r.ClampZ());
	sampler_info.mipLodBias              = static_cast<float>(static_cast<int16_t>((r.LodBias() ^ 0x2000u) - 0x2000u)) / 256.0f;
	sampler_info.anisotropyEnable        = (aniso ? VK_TRUE : VK_FALSE);
	sampler_info.maxAnisotropy           = aniso_ratio;
	sampler_info.compareEnable           = VK_TRUE;
	sampler_info.compareOp               = static_cast<VkCompareOp>(r.DepthCompareFunc());
	sampler_info.minLod                  = min_lod;
	sampler_info.maxLod                  = max_lod;
	sampler_info.borderColor             = border;
	sampler_info.unnormalizedCoordinates = (r.ForceUnormCoords() ? VK_TRUE : VK_FALSE);

	vkCreateSampler(g_render_ctx->GetGraphicCtx()->device, &sampler_info, nullptr, &s.vk);
	EXIT_NOT_IMPLEMENTED(s.vk == nullptr);

	m_samplers.Add(s);
	return m_samplers_size;
}

static VkFormat get_input_format(const ShaderBufferResource& res)
{
	auto nfmt = res.Nfmt();
	auto dfmt = res.Dfmt();
	if (nfmt == 7 && dfmt == 14)
	{
		return VK_FORMAT_R32G32B32A32_SFLOAT;
	}
	if (nfmt == 7 && dfmt == 13)
	{
		return VK_FORMAT_R32G32B32_SFLOAT;
	}
	if (nfmt == 7 && dfmt == 11)
	{
		return VK_FORMAT_R32G32_SFLOAT;
	}
	if (nfmt == 0 && dfmt == 10)
	{
		return VK_FORMAT_R8G8B8A8_UNORM;
	}
	EXIT("unknown format: nfmt = %u, dfmt = %u\n", nfmt, dfmt);
	return VK_FORMAT_UNDEFINED;
}

static VkBlendFactor get_blend_factor(uint32_t factor)
{
	switch (factor)
	{
		case /* Zero */ 0x00000000: return VK_BLEND_FACTOR_ZERO;
		case /* One */ 0x00000001: return VK_BLEND_FACTOR_ONE;
		case /* SrcColor */ 0x00000002: return VK_BLEND_FACTOR_SRC_COLOR;
		case /* OneMinusSrcColor */ 0x00000003: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case /* SrcAlpha */ 0x00000004: return VK_BLEND_FACTOR_SRC_ALPHA;
		case /* OneMinusSrcAlpha */ 0x00000005: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case /* DestAlpha */ 0x00000006: return VK_BLEND_FACTOR_DST_ALPHA;
		case /* OneMinusDestAlpha */ 0x00000007: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case /* DestColor */ 0x00000008: return VK_BLEND_FACTOR_DST_COLOR;
		case /* OneMinusDestColor */ 0x00000009: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		case /* SrcAlphaSaturate */ 0x0000000a: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
		case /* ConstantColor */ 0x0000000d: return VK_BLEND_FACTOR_CONSTANT_COLOR;
		case /* OneMinusConstantColor */ 0x0000000e: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
		case /* Src1Color */ 0x0000000f: return VK_BLEND_FACTOR_SRC1_COLOR;
		case /* InverseSrc1Color */ 0x00000010: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
		case /* Src1Alpha */ 0x00000011: return VK_BLEND_FACTOR_SRC1_ALPHA;
		case /* InverseSrc1Alpha */ 0x00000012: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
		case /* ConstantAlpha */ 0x00000013: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
		case /* OneMinusConstantAlpha */ 0x00000014: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
		default: EXIT("unknown factor: %u\n", factor);
	}
	return VK_BLEND_FACTOR_ZERO;
}

static VkBlendOp get_blend_op(uint32_t op)
{
	switch (op)
	{
		case /* Add */ 0x00000000: return VK_BLEND_OP_ADD;
		case /* Subtract */ 0x00000001: return VK_BLEND_OP_SUBTRACT;
		case /* Min */ 0x00000002: return VK_BLEND_OP_MIN;
		case /* Max */ 0x00000003: return VK_BLEND_OP_MAX;
		case /* ReverseSubtract */ 0x00000004: return VK_BLEND_OP_REVERSE_SUBTRACT;
		default: EXIT("unknown op: %u\n", op);
	}
	return VK_BLEND_OP_ADD;
}

static void CreateLayout(VkDescriptorSetLayout* set_layouts, uint32_t* set_layouts_num, VkPushConstantRange* push_constant_info,
                         uint32_t* push_constant_info_num, const ShaderBindResources& bind, const ShaderBindParameters& bind_params,
                         VkShaderStageFlags vk_stage, DescriptorCache::Stage stage)
{
	EXIT_IF(set_layouts == nullptr);
	EXIT_IF(set_layouts_num == nullptr);
	EXIT_IF(push_constant_info == nullptr);
	EXIT_IF(push_constant_info_num == nullptr);

	bool need_bind = (bind.storage_buffers.buffers_num > 0 || bind.textures2D.textures_num > 0 || bind.samplers.samplers_num > 0 ||
	                  bind.gds_pointers.pointers_num > 0);

	EXIT_IF(need_bind && bind.push_constant_size == 0);

	if (need_bind)
	{
		auto index = *push_constant_info_num;

		push_constant_info[index].stageFlags = vk_stage;
		push_constant_info[index].offset     = bind.push_constant_offset;
		push_constant_info[index].size       = bind.push_constant_size;
		(*push_constant_info_num)++;
	}

	if (need_bind)
	{
		EXIT_IF(bind.descriptor_set_slot != *set_layouts_num);

		set_layouts[*set_layouts_num] = g_render_ctx->GetDescriptorCache()->GetDescriptorSetLayout(stage, bind, bind_params);
		(*set_layouts_num)++;
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static VulkanPipeline* CreatePipelineInternal(VkRenderPass render_pass, const ShaderVertexInputInfo* vs_input_info,
                                              const Vector<uint32_t>& vs_shader, const ShaderPixelInputInfo* ps_input_info,
                                              const Vector<uint32_t>& ps_shader, const PipelineStaticParameters* static_params,
                                              PipelineDynamicParameters*          dynamic_params,
                                              const PipelineAdditionalParameters* additional_params)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(render_pass == nullptr);
	EXIT_IF(static_params == nullptr);
	EXIT_IF(dynamic_params == nullptr);
	EXIT_IF(additional_params == nullptr);

	auto* pipeline              = new VulkanPipeline;
	pipeline->additional_params = additional_params;
	pipeline->static_params     = static_params;
	pipeline->dynamic_params    = dynamic_params;

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	VkShaderModule vert_shader_module = nullptr;
	VkShaderModule frag_shader_module = nullptr;

	VkShaderModuleCreateInfo create_info {};

	create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create_info.pNext = nullptr;
	create_info.flags = 0;

	create_info.codeSize = vs_shader.Size() * 4;
	create_info.pCode    = vs_shader.GetDataConst();
	vkCreateShaderModule(gctx->device, &create_info, nullptr, &vert_shader_module);

	create_info.codeSize = ps_shader.Size() * 4;
	create_info.pCode    = ps_shader.GetDataConst();
	vkCreateShaderModule(gctx->device, &create_info, nullptr, &frag_shader_module);

	EXIT_NOT_IMPLEMENTED(vert_shader_module == nullptr);
	EXIT_NOT_IMPLEMENTED(frag_shader_module == nullptr);

	VkPipelineShaderStageCreateInfo vert_shader_stage_info {};
	vert_shader_stage_info.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	vert_shader_stage_info.pNext               = nullptr;
	vert_shader_stage_info.flags               = 0;
	vert_shader_stage_info.stage               = VK_SHADER_STAGE_VERTEX_BIT;
	vert_shader_stage_info.module              = vert_shader_module;
	vert_shader_stage_info.pName               = "main";
	vert_shader_stage_info.pSpecializationInfo = nullptr;

	VkPipelineShaderStageCreateInfo frag_shader_stage_info {};
	frag_shader_stage_info.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	frag_shader_stage_info.pNext               = nullptr;
	frag_shader_stage_info.flags               = 0;
	frag_shader_stage_info.stage               = VK_SHADER_STAGE_FRAGMENT_BIT;
	frag_shader_stage_info.module              = frag_shader_module;
	frag_shader_stage_info.pName               = "main";
	frag_shader_stage_info.pSpecializationInfo = nullptr;

	VkPipelineShaderStageCreateInfo shader_stages[] = {vert_shader_stage_info, frag_shader_stage_info};

	VkVertexInputAttributeDescription input_attr[ShaderVertexInputInfo::RES_MAX];
	VkVertexInputBindingDescription   input_desc[ShaderVertexInputInfo::RES_MAX];
	for (int bi = 0; bi < vs_input_info->buffers_num; bi++)
	{
		const auto& b            = vs_input_info->buffers[bi];
		input_desc[bi].binding   = bi;
		input_desc[bi].stride    = b.stride;
		input_desc[bi].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		for (int ai = 0; ai < b.attr_num; ai++)
		{
			input_attr[ai].binding  = bi;
			input_attr[ai].location = ai;
			input_attr[ai].offset   = b.attr_offsets[ai];
			input_attr[ai].format   = get_input_format(vs_input_info->resources[ai]);

			auto registers_num = vs_input_info->resources_dst[ai].registers_num;

			EXIT_NOT_IMPLEMENTED(vs_input_info->resources[ai].AddTid());
			EXIT_NOT_IMPLEMENTED(vs_input_info->resources[ai].SwizzleEnabled());
			EXIT_NOT_IMPLEMENTED(vs_input_info->resources[ai].DstSelX() != 4);
			EXIT_NOT_IMPLEMENTED(registers_num >= 2 && vs_input_info->resources[ai].DstSelY() != 5);
			EXIT_NOT_IMPLEMENTED(registers_num >= 3 && vs_input_info->resources[ai].DstSelZ() != 6);
			EXIT_NOT_IMPLEMENTED(registers_num >= 4 && vs_input_info->resources[ai].DstSelW() != 7);
		}
	}

	VkPipelineVertexInputStateCreateInfo vertex_input_info {};
	vertex_input_info.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input_info.pNext                           = nullptr;
	vertex_input_info.flags                           = 0;
	vertex_input_info.vertexBindingDescriptionCount   = vs_input_info->buffers_num;
	vertex_input_info.pVertexBindingDescriptions      = input_desc;
	vertex_input_info.vertexAttributeDescriptionCount = vs_input_info->resources_num;
	vertex_input_info.pVertexAttributeDescriptions    = input_attr;

	VkPipelineInputAssemblyStateCreateInfo input_assembly {};
	input_assembly.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly.pNext                  = nullptr;
	input_assembly.flags                  = 0;
	input_assembly.topology               = static_params->topology;
	input_assembly.primitiveRestartEnable = VK_FALSE;

	VkViewport viewport {};
	viewport.x        = static_params->viewport_offset[0] - static_params->viewport_scale[0];
	viewport.y        = static_params->viewport_offset[1] - static_params->viewport_scale[1];
	viewport.width    = static_params->viewport_scale[0] * 2.0f;
	viewport.height   = static_params->viewport_scale[1] * 2.0f;
	viewport.minDepth = static_params->viewport_offset[2];
	viewport.maxDepth = static_params->viewport_scale[2] + static_params->viewport_offset[2];

	VkRect2D scissor {};
	scissor.offset = {static_params->scissor_ltrb[0], static_params->scissor_ltrb[1]};
	scissor.extent = {static_cast<uint32_t>(static_params->scissor_ltrb[2] - static_params->scissor_ltrb[0]),
	                  static_cast<uint32_t>(static_params->scissor_ltrb[3] - static_params->scissor_ltrb[1])};

	VkPipelineViewportStateCreateInfo viewport_state {};
	viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.pNext         = nullptr;
	viewport_state.flags         = 0;
	viewport_state.viewportCount = 1;
	viewport_state.pViewports    = &viewport;
	viewport_state.scissorCount  = 1;
	viewport_state.pScissors     = &scissor;

	VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
	cull_mode |= (static_params->cull_back ? VK_CULL_MODE_BACK_BIT : 0u);
	cull_mode |= (static_params->cull_front ? VK_CULL_MODE_FRONT_BIT : 0u);

	VkFrontFace front_face = (static_params->face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);

	VkPipelineRasterizationDepthClipStateCreateInfoEXT clip_ext {};
	clip_ext.sType           = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT;
	clip_ext.pNext           = nullptr;
	clip_ext.flags           = 0;
	clip_ext.depthClipEnable = VK_FALSE;

	VkPipelineRasterizationStateCreateInfo rasterizer {};
	rasterizer.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.pNext                   = &clip_ext;
	rasterizer.flags                   = 0;
	rasterizer.depthClampEnable        = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode             = VK_POLYGON_MODE_FILL;
	rasterizer.cullMode                = cull_mode;
	rasterizer.frontFace               = front_face;
	rasterizer.depthBiasEnable         = VK_FALSE;
	rasterizer.depthBiasConstantFactor = 0.0f;
	rasterizer.depthBiasClamp          = 0.0f;
	rasterizer.depthBiasSlopeFactor    = 0.0f;
	rasterizer.lineWidth               = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisampling {};
	multisampling.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.pNext                 = nullptr;
	multisampling.flags                 = 0;
	multisampling.sampleShadingEnable   = VK_FALSE;
	multisampling.rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT;
	multisampling.minSampleShading      = 1.0f;
	multisampling.pSampleMask           = nullptr;
	multisampling.alphaToCoverageEnable = VK_FALSE;
	multisampling.alphaToOneEnable      = VK_FALSE;

	VkColorComponentFlags color_write_mask = 0;

	if (static_params->color_mask == 0xF)
	{
		color_write_mask =
		    static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_R_BIT) | static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_G_BIT) |
		    static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_B_BIT) | static_cast<VkColorComponentFlags>(VK_COLOR_COMPONENT_A_BIT);
	} else if (static_params->color_mask == 0x0)
	{
		color_write_mask = 0;
	} else
	{
		EXIT("unknown mask: %u\n", static_params->color_mask);
	}

	VkPipelineColorBlendAttachmentState color_blend_attachment {};
	color_blend_attachment.colorWriteMask      = color_write_mask;
	color_blend_attachment.blendEnable         = static_params->blend_enable ? VK_TRUE : VK_FALSE;
	color_blend_attachment.srcColorBlendFactor = get_blend_factor(static_params->color_srcblend);
	color_blend_attachment.dstColorBlendFactor = get_blend_factor(static_params->color_destblend);
	color_blend_attachment.colorBlendOp        = get_blend_op(static_params->color_comb_fcn);
	color_blend_attachment.srcAlphaBlendFactor = (static_params->separate_alpha_blend ? get_blend_factor(static_params->alpha_srcblend)
	                                                                                  : color_blend_attachment.srcColorBlendFactor);
	color_blend_attachment.dstAlphaBlendFactor = (static_params->separate_alpha_blend ? get_blend_factor(static_params->alpha_destblend)
	                                                                                  : color_blend_attachment.dstColorBlendFactor);
	color_blend_attachment.alphaBlendOp =
	    (static_params->separate_alpha_blend ? get_blend_op(static_params->alpha_comb_fcn) : color_blend_attachment.colorBlendOp);

	VkPipelineColorBlendStateCreateInfo color_blending {};
	color_blending.sType             = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	color_blending.pNext             = nullptr;
	color_blending.flags             = 0;
	color_blending.logicOpEnable     = VK_FALSE;
	color_blending.logicOp           = VK_LOGIC_OP_COPY;
	color_blending.attachmentCount   = 1;
	color_blending.pAttachments      = &color_blend_attachment;
	color_blending.blendConstants[0] = static_params->blend_color_red;
	color_blending.blendConstants[1] = static_params->blend_color_green;
	color_blending.blendConstants[2] = static_params->blend_color_blue;
	color_blending.blendConstants[3] = static_params->blend_color_alpha;

	VkDescriptorSetLayout set_layouts[2]  = {};
	uint32_t              set_layouts_num = 0;

	VkPushConstantRange push_constant_info[2];
	uint32_t            push_constant_info_num = 0;

	CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num, vs_input_info->bind,
	             additional_params->vs_bind, VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex);
	CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num, ps_input_info->bind,
	             additional_params->ps_bind, VK_SHADER_STAGE_FRAGMENT_BIT, DescriptorCache::Stage::Pixel);

	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.pNext                  = nullptr;
	pipeline_layout_info.flags                  = 0;
	pipeline_layout_info.setLayoutCount         = set_layouts_num;
	pipeline_layout_info.pSetLayouts            = (set_layouts_num > 0 ? set_layouts : nullptr);
	pipeline_layout_info.pushConstantRangeCount = push_constant_info_num;
	pipeline_layout_info.pPushConstantRanges    = push_constant_info_num > 0 ? push_constant_info : nullptr;

	EXIT_IF(pipeline->pipeline_layout != nullptr);

	vkCreatePipelineLayout(gctx->device, &pipeline_layout_info, nullptr, &pipeline->pipeline_layout);

	EXIT_NOT_IMPLEMENTED(pipeline->pipeline_layout == nullptr);

	VkPipelineDepthStencilStateCreateInfo depth_stencil_info {};
	depth_stencil_info.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth_stencil_info.pNext                 = nullptr;
	depth_stencil_info.flags                 = 0;
	depth_stencil_info.depthTestEnable       = (static_params->depth_test_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.depthWriteEnable      = (static_params->depth_write_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.depthCompareOp        = static_params->depth_compare_op;
	depth_stencil_info.depthBoundsTestEnable = (static_params->depth_bounds_test_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.stencilTestEnable     = (static_params->stencil_test_enable ? VK_TRUE : VK_FALSE);
	depth_stencil_info.front.failOp          = static_params->stencil_front.failOp;
	depth_stencil_info.front.passOp          = static_params->stencil_front.passOp;
	depth_stencil_info.front.depthFailOp     = static_params->stencil_front.depthFailOp;
	depth_stencil_info.front.compareOp       = static_params->stencil_front.compareOp;
	depth_stencil_info.front.compareMask     = dynamic_params->stencil_front.compareMask;
	depth_stencil_info.front.writeMask       = dynamic_params->stencil_front.writeMask;
	depth_stencil_info.front.reference       = dynamic_params->stencil_front.reference;
	depth_stencil_info.back.failOp           = static_params->stencil_back.failOp;
	depth_stencil_info.back.passOp           = static_params->stencil_back.passOp;
	depth_stencil_info.back.depthFailOp      = static_params->stencil_back.depthFailOp;
	depth_stencil_info.back.compareOp        = static_params->stencil_back.compareOp;
	depth_stencil_info.back.compareMask      = dynamic_params->stencil_back.compareMask;
	depth_stencil_info.back.writeMask        = dynamic_params->stencil_back.writeMask;
	depth_stencil_info.back.reference        = dynamic_params->stencil_back.reference;
	depth_stencil_info.minDepthBounds        = static_params->depth_min_bounds;
	depth_stencil_info.maxDepthBounds        = static_params->depth_max_bounds;

	VkDynamicState dynamic_states[8]    = {};
	uint32_t       dynamic_states_count = 0;
	if (dynamic_params->vk_dynamic_state_stencil_compare_mask)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
	}
	if (dynamic_params->vk_dynamic_state_stencil_reference)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
	}
	if (dynamic_params->vk_dynamic_state_stencil_write_mask)
	{
		dynamic_states[dynamic_states_count++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
	}

	VkPipelineDynamicStateCreateInfo dynamic_state {};
	dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state.pNext             = nullptr;
	dynamic_state.flags             = 0;
	dynamic_state.dynamicStateCount = dynamic_states_count;
	dynamic_state.pDynamicStates    = dynamic_states;

	VkGraphicsPipelineCreateInfo pipeline_info {};
	pipeline_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline_info.pNext               = nullptr;
	pipeline_info.flags               = 0;
	pipeline_info.stageCount          = 2;
	pipeline_info.pStages             = shader_stages;
	pipeline_info.pVertexInputState   = &vertex_input_info;
	pipeline_info.pInputAssemblyState = &input_assembly;
	pipeline_info.pTessellationState  = nullptr;
	pipeline_info.pViewportState      = &viewport_state;
	pipeline_info.pRasterizationState = &rasterizer;
	pipeline_info.pMultisampleState   = &multisampling;
	pipeline_info.pDepthStencilState  = (static_params->with_depth ? &depth_stencil_info : nullptr);
	pipeline_info.pColorBlendState    = &color_blending;
	pipeline_info.pDynamicState       = &dynamic_state;
	pipeline_info.layout              = pipeline->pipeline_layout;
	pipeline_info.renderPass          = render_pass;
	pipeline_info.subpass             = 0;
	pipeline_info.basePipelineHandle  = nullptr;
	pipeline_info.basePipelineIndex   = -1;

	EXIT_IF(pipeline->pipeline != nullptr);

	vkCreateGraphicsPipelines(gctx->device, nullptr, 1, &pipeline_info, nullptr, &pipeline->pipeline);

	EXIT_NOT_IMPLEMENTED(pipeline->pipeline == nullptr);

	vkDestroyShaderModule(gctx->device, frag_shader_module, nullptr);
	vkDestroyShaderModule(gctx->device, vert_shader_module, nullptr);

	return pipeline;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static VulkanPipeline* CreatePipelineInternal(const ShaderComputeInputInfo* input_info, const Vector<uint32_t>& cs_shader,
                                              const PipelineStaticParameters* static_params, PipelineDynamicParameters* dynamic_params,
                                              const PipelineAdditionalParameters* additional_params)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(static_params == nullptr);
	EXIT_IF(dynamic_params == nullptr);
	EXIT_IF(additional_params == nullptr);

	auto* pipeline              = new VulkanPipeline;
	pipeline->additional_params = additional_params;
	pipeline->static_params     = static_params;
	pipeline->dynamic_params    = dynamic_params;

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	VkShaderModule comp_shader_module = nullptr;

	VkShaderModuleCreateInfo create_info {};

	create_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create_info.pNext    = nullptr;
	create_info.flags    = 0;
	create_info.codeSize = cs_shader.Size() * 4;
	create_info.pCode    = cs_shader.GetDataConst();
	vkCreateShaderModule(gctx->device, &create_info, nullptr, &comp_shader_module);

	EXIT_NOT_IMPLEMENTED(comp_shader_module == nullptr);

	VkPipelineShaderStageCreateInfo comp_shader_stage_info {};
	comp_shader_stage_info.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	comp_shader_stage_info.pNext               = nullptr;
	comp_shader_stage_info.flags               = 0;
	comp_shader_stage_info.stage               = VK_SHADER_STAGE_COMPUTE_BIT;
	comp_shader_stage_info.module              = comp_shader_module;
	comp_shader_stage_info.pName               = "main";
	comp_shader_stage_info.pSpecializationInfo = nullptr;

	VkDescriptorSetLayout set_layouts[1]  = {};
	uint32_t              set_layouts_num = 0;

	VkPushConstantRange push_constant_info[1];
	uint32_t            push_constant_info_num = 0;

	CreateLayout(set_layouts, &set_layouts_num, push_constant_info, &push_constant_info_num, input_info->bind, additional_params->cs_bind,
	             VK_SHADER_STAGE_COMPUTE_BIT, DescriptorCache::Stage::Compute);

	VkPipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout_info.pNext                  = nullptr;
	pipeline_layout_info.flags                  = 0;
	pipeline_layout_info.setLayoutCount         = set_layouts_num;
	pipeline_layout_info.pSetLayouts            = (set_layouts_num > 0 ? set_layouts : nullptr);
	pipeline_layout_info.pushConstantRangeCount = push_constant_info_num;
	pipeline_layout_info.pPushConstantRanges    = push_constant_info_num > 0 ? push_constant_info : nullptr;

	EXIT_IF(pipeline->pipeline_layout != nullptr);

	vkCreatePipelineLayout(gctx->device, &pipeline_layout_info, nullptr, &pipeline->pipeline_layout);

	EXIT_NOT_IMPLEMENTED(pipeline->pipeline_layout == nullptr);

	VkComputePipelineCreateInfo info {};
	info.sType              = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.pNext              = nullptr;
	info.flags              = 0;
	info.stage              = comp_shader_stage_info;
	info.layout             = pipeline->pipeline_layout;
	info.basePipelineHandle = nullptr;
	info.basePipelineIndex  = -1;

	EXIT_IF(pipeline->pipeline != nullptr);

	vkCreateComputePipelines(gctx->device, nullptr, 1, &info, nullptr, &pipeline->pipeline);

	EXIT_NOT_IMPLEMENTED(pipeline->pipeline == nullptr);

	vkDestroyShaderModule(gctx->device, comp_shader_module, nullptr);

	return pipeline;
}

void PipelineCache::DeletePipelineInternal(Pipeline& p)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(p.pipeline == nullptr);
	EXIT_IF(p.pipeline->pipeline == nullptr);
	EXIT_IF(p.pipeline->pipeline_layout == nullptr);
	EXIT_IF(p.static_params == nullptr);
	EXIT_IF(p.dynamic_params == nullptr);
	EXIT_IF(p.pipeline->additional_params == nullptr);

	EXIT_IF(p.pipeline->static_params != p.static_params);
	EXIT_IF(p.pipeline->dynamic_params != p.dynamic_params);

	delete p.static_params;
	delete p.dynamic_params;
	delete p.pipeline->additional_params;

	p.static_params               = nullptr;
	p.dynamic_params              = nullptr;
	p.pipeline->additional_params = nullptr;

	auto* gctx = g_render_ctx->GetGraphicCtx();

	EXIT_IF(gctx == nullptr);

	vkDestroyPipeline(gctx->device, p.pipeline->pipeline, nullptr);
	vkDestroyPipelineLayout(gctx->device, p.pipeline->pipeline_layout, nullptr);

	delete p.pipeline;

	p.pipeline = nullptr;
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const
{
	return (0 == memcmp(this, &other, sizeof(struct PipelineStaticParameters)));
}

bool PipelineDynamicParameters::operator==(const PipelineDynamicParameters& other) const
{
	EXIT_IF(vk_dynamic_state_stencil_compare_mask != other.vk_dynamic_state_stencil_compare_mask ||
	        vk_dynamic_state_stencil_write_mask != other.vk_dynamic_state_stencil_write_mask ||
	        vk_dynamic_state_stencil_reference != other.vk_dynamic_state_stencil_reference);

	if (!vk_dynamic_state_stencil_compare_mask)
	{
		if (stencil_front.compareMask != other.stencil_front.compareMask || stencil_back.compareMask != other.stencil_back.compareMask)
		{
			return false;
		}
	}
	if (!vk_dynamic_state_stencil_write_mask)
	{
		if (stencil_front.writeMask != other.stencil_front.writeMask || stencil_back.writeMask != other.stencil_back.writeMask)
		{
			return false;
		}
	}
	if (!vk_dynamic_state_stencil_reference)
	{
		if (stencil_front.reference != other.stencil_front.reference || stencil_back.reference != other.stencil_back.reference)
		{
			return false;
		}
	}
	return true;
}

VulkanPipeline* PipelineCache::Find(const Pipeline& p) const
{
	for (const auto& pn: m_pipelines)
	{
		if (pn.pipeline != nullptr && p.render_pass_id == pn.render_pass_id && p.vs_shader_id == pn.vs_shader_id &&
		    p.ps_shader_id == pn.ps_shader_id && p.cs_shader_id == pn.cs_shader_id && *p.static_params == *pn.static_params &&
		    *p.dynamic_params == *pn.dynamic_params)
		{
			return pn.pipeline;
		}
	}
	return nullptr;
}

VulkanPipeline* PipelineCache::CreatePipeline(VulkanFramebuffer* framebuffer, RenderColorInfo* color, RenderDepthInfo* depth,
                                              const ShaderVertexInputInfo* vs_input_info, const VertexShaderInfo* vs_regs,
                                              const ShaderPixelInputInfo* ps_input_info, const PixelShaderInfo* ps_regs,
                                              VkPrimitiveTopology topology, uint32_t color_mask, const ModeControl& mc,
                                              const BlendControl& bc, const BlendColor& bclr, const ScreenViewport& vp)
{
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(framebuffer == nullptr);
	EXIT_IF(vs_regs == nullptr);
	EXIT_IF(ps_regs == nullptr);
	EXIT_IF(depth == nullptr);
	EXIT_IF(color == nullptr);

	Core::LockGuard lock(m_mutex);

	if (Config::GetPrintfDirection() != Log::Direction::Silent)
	{
		ShaderDbgDumpInputInfo(vs_input_info);
		ShaderDbgDumpInputInfo(ps_input_info);
	}

	auto vs_id = ShaderGetIdVS(vs_regs, vs_input_info);
	auto ps_id = ShaderGetIdPS(ps_regs, ps_input_info);

	Pipeline p {};
	p.render_pass_id = framebuffer->render_pass_id;
	p.ps_shader_id   = ps_id;
	p.vs_shader_id   = vs_id;
	p.static_params  = new PipelineStaticParameters;
	p.dynamic_params = new PipelineDynamicParameters;

	p.dynamic_params->vk_dynamic_state_stencil_compare_mask = true;
	p.dynamic_params->vk_dynamic_state_stencil_reference    = true;
	p.dynamic_params->vk_dynamic_state_stencil_write_mask   = true;

	EXIT_NOT_IMPLEMENTED(depth->depth_test_enable && ps_input_info->ps_execute_on_noop);

	p.static_params->viewport_scale[0]        = vp.viewports[0].xscale;
	p.static_params->viewport_scale[1]        = vp.viewports[0].yscale;
	p.static_params->viewport_scale[2]        = vp.viewports[0].zscale;
	p.static_params->viewport_offset[0]       = vp.viewports[0].xoffset;
	p.static_params->viewport_offset[1]       = vp.viewports[0].yoffset;
	p.static_params->viewport_offset[2]       = vp.viewports[0].zoffset;
	p.static_params->scissor_ltrb[0]          = vp.scissor_left;
	p.static_params->scissor_ltrb[1]          = vp.scissor_top;
	p.static_params->scissor_ltrb[2]          = vp.scissor_right;
	p.static_params->scissor_ltrb[3]          = vp.scissor_bottom;
	p.static_params->topology                 = topology;
	p.static_params->with_depth               = (depth->format != VK_FORMAT_UNDEFINED && depth->vulkan_buffer != nullptr);
	p.static_params->depth_test_enable        = depth->depth_test_enable;
	p.static_params->depth_write_enable       = (depth->depth_write_enable && !depth->depth_clear_enable);
	p.static_params->depth_compare_op         = depth->depth_compare_op;
	p.static_params->depth_bounds_test_enable = depth->depth_bounds_test_enable;
	p.static_params->depth_min_bounds         = depth->depth_min_bounds;
	p.static_params->depth_max_bounds         = depth->depth_max_bounds;
	p.static_params->stencil_test_enable      = depth->stencil_test_enable;
	p.static_params->stencil_front            = depth->stencil_static_front;
	p.static_params->stencil_back             = depth->stencil_static_back;
	p.static_params->color_mask               = color_mask;
	p.static_params->cull_back                = mc.cull_back;
	p.static_params->cull_front               = mc.cull_front;
	p.static_params->face                     = mc.face;
	p.static_params->color_srcblend           = bc.color_srcblend;
	p.static_params->color_comb_fcn           = bc.color_comb_fcn;
	p.static_params->color_destblend          = bc.color_destblend;
	p.static_params->alpha_srcblend           = bc.alpha_srcblend;
	p.static_params->alpha_comb_fcn           = bc.alpha_comb_fcn;
	p.static_params->alpha_destblend          = bc.alpha_destblend;
	p.static_params->separate_alpha_blend     = bc.separate_alpha_blend;
	p.static_params->blend_enable             = bc.enable;
	p.static_params->blend_color_red          = bclr.red;
	p.static_params->blend_color_green        = bclr.green;
	p.static_params->blend_color_blue         = bclr.blue;
	p.static_params->blend_color_alpha        = bclr.alpha;

	p.dynamic_params->stencil_front = depth->stencil_dynamic_front;
	p.dynamic_params->stencil_back  = depth->stencil_dynamic_back;

	auto* found = Find(p);

	if (found != nullptr)
	{
		*found->dynamic_params = *p.dynamic_params;
		delete p.static_params;
		delete p.dynamic_params;
		return found;
	}

	auto vs_code = ShaderParseVS(vs_regs);
	auto ps_code = ShaderParsePS(ps_regs);

	auto* params2 = new PipelineAdditionalParameters;

	params2->vs_bind = ShaderGetBindParametersVS(vs_code, vs_input_info);
	params2->ps_bind = ShaderGetBindParametersPS(ps_code, ps_input_info);

	auto vs_shader = ShaderRecompileVS(vs_code, vs_input_info);
	auto ps_shader = ShaderRecompilePS(ps_code, ps_input_info);

	EXIT_IF(vs_shader.IsEmpty());
	EXIT_IF(ps_shader.IsEmpty());

	p.pipeline = CreatePipelineInternal(framebuffer->render_pass, vs_input_info, vs_shader, ps_input_info, ps_shader, p.static_params,
	                                    p.dynamic_params, params2);

	EXIT_NOT_IMPLEMENTED(p.pipeline == nullptr);

	bool updated = false;
	for (auto& pn: m_pipelines)
	{
		if (pn.pipeline == nullptr)
		{
			pn      = p;
			updated = true;
			break;
		}
	}

	if (!updated)
	{
		if (m_pipelines.Size() >= PipelineCache::MAX_PIPELINES)
		{
			auto& pn = m_pipelines[Math::Rand::UintInclusiveRange(0, m_pipelines.Size() - 1)];
			DeletePipelineInternal(pn);
			pn = p;
		} else
		{
			m_pipelines.Add(p);
		}
	}

	return p.pipeline;
}

VulkanPipeline* PipelineCache::CreatePipeline(const ShaderComputeInputInfo* input_info, const ComputeShaderInfo* cs_regs)
{
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(cs_regs == nullptr);

	Core::LockGuard lock(m_mutex);

	ShaderDbgDumpInputInfo(input_info);

	auto cs_id = ShaderGetIdCS(cs_regs, input_info);

	Pipeline p {};
	p.cs_shader_id   = cs_id;
	p.static_params  = new PipelineStaticParameters;
	p.dynamic_params = new PipelineDynamicParameters;

	p.dynamic_params->vk_dynamic_state_stencil_compare_mask = true;
	p.dynamic_params->vk_dynamic_state_stencil_reference    = true;
	p.dynamic_params->vk_dynamic_state_stencil_write_mask   = true;

	auto* found = Find(p);

	if (found != nullptr)
	{
		*found->dynamic_params = *p.dynamic_params;
		delete p.static_params;
		delete p.dynamic_params;
		return found;
	}

	auto cs_code = ShaderParseCS(cs_regs);

	auto* params2 = new PipelineAdditionalParameters;

	params2->cs_bind = ShaderGetBindParametersCS(cs_code, input_info);

	auto cs_shader = ShaderRecompileCS(cs_code, input_info);
	EXIT_IF(cs_shader.IsEmpty());

	p.pipeline = CreatePipelineInternal(input_info, cs_shader, p.static_params, p.dynamic_params, params2);

	EXIT_NOT_IMPLEMENTED(p.pipeline == nullptr);

	bool updated = false;
	for (auto& pn: m_pipelines)
	{
		if (pn.pipeline == nullptr)
		{
			pn      = p;
			updated = true;
			break;
		}
	}

	if (!updated)
	{
		if (m_pipelines.Size() >= PipelineCache::MAX_PIPELINES)
		{
			auto& pn = m_pipelines[Math::Rand::UintInclusiveRange(0, m_pipelines.Size() - 1)];
			DeletePipelineInternal(pn);
			pn = p;
		} else
		{
			m_pipelines.Add(p);
		}
	}

	return p.pipeline;
}

void PipelineCache::DeletePipeline(VulkanPipeline* pipeline)
{
	Core::LockGuard lock(m_mutex);

	auto index = m_pipelines.Find(pipeline, [](auto r, auto p) { return p == r.pipeline; });

	EXIT_IF(!m_pipelines.IndexValid(index));

	if (m_pipelines.IndexValid(index))
	{
		DeletePipelineInternal(m_pipelines[index]);
		// m_pipelines.RemoveAt(index);
		m_pipelines[index].pipeline = nullptr;
	}
}

void PipelineCache::DeletePipelines(VulkanFramebuffer* framebuffer)
{
	EXIT_IF(framebuffer == nullptr);

	Core::LockGuard lock(m_mutex);

	for (auto& p: m_pipelines)
	{
		if (p.pipeline != nullptr && p.render_pass_id == framebuffer->render_pass_id)
		{
			DeletePipelineInternal(p);
			p.pipeline = nullptr;
		}
	}
}

void PipelineCache::DeleteAllPipelines()
{
	Core::LockGuard lock(m_mutex);

	for (auto& p: m_pipelines)
	{
		DeletePipelineInternal(p);
		p.pipeline = nullptr;
	}

	// m_pipelines.Clear();
}

static void create_layout(GraphicContext* gctx, int storage_buffers_num, int textures2d_sampled_num, int textures2d_storage_num,
                          int samplers_num, int gds_buffers_num, VkShaderStageFlags stage, VkDescriptorSetLayout* dst)
{
	uint32_t binding_num = 0;

	ShaderBindResources tmp {};
	tmp.storage_buffers.buffers_num = storage_buffers_num;
	tmp.textures2D.textures_num     = textures2d_sampled_num + textures2d_storage_num;
	tmp.samplers.samplers_num       = samplers_num;
	tmp.gds_pointers.pointers_num   = gds_buffers_num;

	ShaderCalcBindingIndices(&tmp);

	constexpr uint32_t B_MAX = 5;

	VkDescriptorSetLayoutBinding ubo_layout_binding[B_MAX] = {};

	if (storage_buffers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.storage_buffers.binding_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		ubo_layout_binding[binding_num].descriptorCount    = storage_buffers_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (textures2d_sampled_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.textures2D.binding_sampled_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		ubo_layout_binding[binding_num].descriptorCount    = textures2d_sampled_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (textures2d_storage_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.textures2D.binding_storage_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		ubo_layout_binding[binding_num].descriptorCount    = textures2d_storage_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (samplers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.samplers.binding_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLER;
		ubo_layout_binding[binding_num].descriptorCount    = samplers_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (gds_buffers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		ubo_layout_binding[binding_num].binding            = tmp.gds_pointers.binding_index;
		ubo_layout_binding[binding_num].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		ubo_layout_binding[binding_num].descriptorCount    = gds_buffers_num;
		ubo_layout_binding[binding_num].stageFlags         = stage;
		ubo_layout_binding[binding_num].pImmutableSamplers = nullptr;
		binding_num++;
	}

	if (binding_num > 0)
	{
		VkDescriptorSetLayoutCreateInfo layout_info {};
		layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layout_info.pNext        = nullptr;
		layout_info.flags        = 0;
		layout_info.bindingCount = binding_num;
		layout_info.pBindings    = ubo_layout_binding;

		EXIT_IF(*dst != nullptr);

		vkCreateDescriptorSetLayout(gctx->device, &layout_info, nullptr, dst);

		EXIT_NOT_IMPLEMENTED(*dst == nullptr);
	} else
	{
		*dst = nullptr;
	}
};

void DescriptorCache::Init()
{
	if (m_initialized)
	{
		return;
	}

	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);

	EXIT_IF(m_descriptor_set_layout_vertex[0][0][0][0][0] != nullptr);
	EXIT_IF(m_descriptor_set_layout_pixel[0][0][0][0][0] != nullptr);
	EXIT_IF(m_descriptor_set_layout_compute[0][0][0][0][0] != nullptr);

	for (int buffers_num_i = 0; buffers_num_i <= BUFFERS_MAX; buffers_num_i++)
	{
		for (int buffers_num_j = 0; buffers_num_j <= TEXTURES_SAMPLED_MAX; buffers_num_j++)
		{
			for (int buffers_num_j2 = 0; buffers_num_j2 <= TEXTURES_STORAGE_MAX; buffers_num_j2++)
			{
				for (int buffers_num_k = 0; buffers_num_k <= SAMPLERS_MAX; buffers_num_k++)
				{
					for (int buffers_num_l = 0; buffers_num_l <= GDS_BUFFER_MAX; buffers_num_l++)
					{
						create_layout(
						    gctx, buffers_num_i, buffers_num_j, buffers_num_j2, buffers_num_k, buffers_num_l, VK_SHADER_STAGE_FRAGMENT_BIT,
						    &m_descriptor_set_layout_pixel[buffers_num_i][buffers_num_j][buffers_num_j2][buffers_num_k][buffers_num_l]);
						create_layout(
						    gctx, buffers_num_i, buffers_num_j, buffers_num_j2, buffers_num_k, buffers_num_l, VK_SHADER_STAGE_VERTEX_BIT,
						    &m_descriptor_set_layout_vertex[buffers_num_i][buffers_num_j][buffers_num_j2][buffers_num_k][buffers_num_l]);
						create_layout(
						    gctx, buffers_num_i, buffers_num_j, buffers_num_j2, buffers_num_k, buffers_num_l, VK_SHADER_STAGE_COMPUTE_BIT,
						    &m_descriptor_set_layout_compute[buffers_num_i][buffers_num_j][buffers_num_j2][buffers_num_k][buffers_num_l]);
					}
				}
			}
		}
	}

	m_initialized = true;
}

void DescriptorCache::CreatePool()
{
	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);

	VkDescriptorPoolSize pool_size[4];
	pool_size[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_size[0].descriptorCount = 32;
	pool_size[1].type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	pool_size[1].descriptorCount = 32;
	pool_size[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	pool_size[2].descriptorCount = 32;
	pool_size[3].type            = VK_DESCRIPTOR_TYPE_SAMPLER;
	pool_size[3].descriptorCount = 32;

	VkDescriptorPoolCreateInfo pool_info {};
	pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.pNext         = nullptr;
	pool_info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_info.poolSizeCount = 4;
	pool_info.pPoolSizes    = pool_size;
	pool_info.maxSets       = 32;

	VkDescriptorPool pool = nullptr;

	vkCreateDescriptorPool(gctx->device, &pool_info, nullptr, &pool);

	EXIT_NOT_IMPLEMENTED(pool == nullptr);

	m_pools.Add(pool);
}

VulkanDescriptorSet* DescriptorCache::Allocate(Stage stage, int storage_buffers_num, int textures2d_sampled_num, int textures2d_storage_num,
                                               int samplers_num, int gds_buffers_num)
{
	EXIT_IF(storage_buffers_num < 0 || storage_buffers_num > BUFFERS_MAX);
	EXIT_IF(textures2d_sampled_num < 0 || textures2d_sampled_num > TEXTURES_SAMPLED_MAX);
	EXIT_IF(textures2d_storage_num < 0 || textures2d_storage_num > TEXTURES_STORAGE_MAX);
	EXIT_IF(samplers_num < 0 || samplers_num > SAMPLERS_MAX);
	EXIT_IF(gds_buffers_num < 0 || gds_buffers_num > GDS_BUFFER_MAX);

	Core::LockGuard lock(m_mutex);

	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);

	Init();

	auto* ret = new VulkanDescriptorSet;

	ret->set = nullptr;

	for (int try_num = 0; try_num < 2; try_num++)
	{
		for (auto* pool: m_pools)
		{
			VkDescriptorSetAllocateInfo alloc_info {};
			alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			alloc_info.pNext              = nullptr;
			alloc_info.descriptorPool     = pool;
			alloc_info.descriptorSetCount = 1;

			switch (stage)
			{
				case Stage::Vertex:
					alloc_info.pSetLayouts = &m_descriptor_set_layout_vertex[storage_buffers_num][textures2d_sampled_num]
					                                                        [textures2d_storage_num][samplers_num][gds_buffers_num];
					break;
				case Stage::Pixel:
					alloc_info.pSetLayouts = &m_descriptor_set_layout_pixel[storage_buffers_num][textures2d_sampled_num]
					                                                       [textures2d_storage_num][samplers_num][gds_buffers_num];
					break;
				case Stage::Compute:
					alloc_info.pSetLayouts = &m_descriptor_set_layout_compute[storage_buffers_num][textures2d_sampled_num]
					                                                         [textures2d_storage_num][samplers_num][gds_buffers_num];
					break;
				default: EXIT("unknown stage\n");
			}

			ret->pool   = pool;
			ret->layout = *alloc_info.pSetLayouts;

			auto result = vkAllocateDescriptorSets(gctx->device, &alloc_info, &ret->set);

			if (result == VK_SUCCESS)
			{
				return ret;
			}
		}

		CreatePool();
	}

	delete ret;
	return nullptr;
}

void DescriptorCache::Free(VulkanDescriptorSet* set)
{
	EXIT_IF(set == nullptr);

	Core::LockGuard lock(m_mutex);

	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);

	vkFreeDescriptorSets(gctx->device, set->pool, 1, &set->set);

	delete set;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
VulkanDescriptorSet* DescriptorCache::GetDescriptor(Stage stage, VulkanBuffer** storage_buffers, VulkanImage** textures2d_sampled,
                                                    VulkanImage** textures2d_storage, uint64_t* samplers, VulkanBuffer** gds_buffers,
                                                    const ShaderBindResources& bind, const ShaderBindParameters& bind_params)
{
	KYTY_PROFILER_BLOCK("DescriptorCache::GetDescriptor");

	int storage_buffers_num    = bind.storage_buffers.buffers_num;
	int textures2d_sampled_num = bind_params.textures2d_sampled_num;
	int textures2d_storage_num = bind_params.textures2d_storage_num;
	int samplers_num           = bind.samplers.samplers_num;
	int gds_buffers_num        = bind.gds_pointers.pointers_num;

	EXIT_IF(storage_buffers_num < 0 || storage_buffers_num > BUFFERS_MAX);
	EXIT_IF(textures2d_sampled_num < 0 || textures2d_sampled_num > TEXTURES_SAMPLED_MAX);
	EXIT_IF(textures2d_storage_num < 0 || textures2d_storage_num > TEXTURES_STORAGE_MAX);
	EXIT_IF(samplers_num < 0 || samplers_num > SAMPLERS_MAX);
	EXIT_IF(storage_buffers == nullptr);

	Core::LockGuard lock(m_mutex);

	auto* gctx = g_render_ctx->GetGraphicCtx();
	EXIT_IF(gctx == nullptr);

	for (auto& set: m_sets)
	{
		if (set.set != nullptr && set.stage == stage && set.storage_buffers_num == storage_buffers_num &&
		    set.textures2d_sampled_num == textures2d_sampled_num && set.textures2d_storage_num == textures2d_storage_num &&
		    set.samplers_num == samplers_num && set.gds_buffers_num == gds_buffers_num)
		{
			bool match = true;
			for (int i = 0; i < storage_buffers_num; i++)
			{
				if (match && storage_buffers[i]->memory.unique_id != set.storage_buffers_id[i])
				{
					match = false;
					break;
				}
			}
			if (match)
			{
				for (int i = 0; i < textures2d_sampled_num; i++)
				{
					if (textures2d_sampled[i]->memory.unique_id != set.textures2d_sampled_id[i])
					{
						match = false;
						break;
					}
				}
			}
			if (match)
			{
				for (int i = 0; i < textures2d_storage_num; i++)
				{
					if (textures2d_storage[i]->memory.unique_id != set.textures2d_storage_id[i])
					{
						match = false;
						break;
					}
				}
			}
			if (match)
			{
				for (int i = 0; i < samplers_num; i++)
				{
					if (samplers[i] != set.samplers_id[i])
					{
						match = false;
						break;
					}
				}
			}
			if (match)
			{
				for (int i = 0; i < gds_buffers_num; i++)
				{
					if (gds_buffers[i]->memory.unique_id != set.gds_buffers_id[i])
					{
						match = false;
						break;
					}
				}
			}
			if (match)
			{
				return set.set;
			}
		}
	}

	auto* new_set = Allocate(stage, storage_buffers_num, textures2d_sampled_num, textures2d_storage_num, samplers_num, gds_buffers_num);
	EXIT_NOT_IMPLEMENTED(new_set == nullptr);

	VkDescriptorBufferInfo buffer_info[BUFFERS_MAX] {};
	for (int i = 0; i < storage_buffers_num; i++)
	{
		buffer_info[i].buffer = storage_buffers[i]->buffer;
		buffer_info[i].offset = 0;
		buffer_info[i].range  = VK_WHOLE_SIZE;
	}

	VkDescriptorImageInfo texture2d_sampled_info[TEXTURES_SAMPLED_MAX] {};
	for (int i = 0; i < textures2d_sampled_num; i++)
	{
		texture2d_sampled_info[i].sampler     = nullptr;
		texture2d_sampled_info[i].imageView   = textures2d_sampled[i]->image_view;
		texture2d_sampled_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkDescriptorImageInfo texture2d_storage_info[TEXTURES_STORAGE_MAX] {};
	for (int i = 0; i < textures2d_storage_num; i++)
	{
		texture2d_storage_info[i].sampler     = nullptr;
		texture2d_storage_info[i].imageView   = textures2d_storage[i]->image_view;
		texture2d_storage_info[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	}

	VkDescriptorImageInfo sampler_info[SAMPLERS_MAX] {};
	for (int i = 0; i < samplers_num; i++)
	{
		sampler_info[i].sampler     = g_render_ctx->GetSamplerCache()->GetSampler(samplers[i]);
		sampler_info[i].imageView   = nullptr;
		sampler_info[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkDescriptorBufferInfo gds_buffer_info[GDS_BUFFER_MAX] {};
	for (int i = 0; i < gds_buffers_num; i++)
	{
		gds_buffer_info[i].buffer = gds_buffers[i]->buffer;
		gds_buffer_info[i].offset = 0;
		gds_buffer_info[i].range  = VK_WHOLE_SIZE;
	}

	int binding_num = 0;

	constexpr uint32_t B_MAX = 5;

	VkWriteDescriptorSet descriptor_write[B_MAX] = {};

	if (storage_buffers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.storage_buffers.binding_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		descriptor_write[binding_num].descriptorCount  = storage_buffers_num;
		descriptor_write[binding_num].pBufferInfo      = buffer_info;
		descriptor_write[binding_num].pImageInfo       = nullptr;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (textures2d_sampled_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.textures2D.binding_sampled_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		descriptor_write[binding_num].descriptorCount  = textures2d_sampled_num;
		descriptor_write[binding_num].pBufferInfo      = nullptr;
		descriptor_write[binding_num].pImageInfo       = texture2d_sampled_info;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (textures2d_storage_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.textures2D.binding_storage_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		descriptor_write[binding_num].descriptorCount  = textures2d_storage_num;
		descriptor_write[binding_num].pBufferInfo      = nullptr;
		descriptor_write[binding_num].pImageInfo       = texture2d_storage_info;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (samplers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.samplers.binding_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLER;
		descriptor_write[binding_num].descriptorCount  = samplers_num;
		descriptor_write[binding_num].pBufferInfo      = nullptr;
		descriptor_write[binding_num].pImageInfo       = sampler_info;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	if (gds_buffers_num > 0)
	{
		EXIT_IF(binding_num >= B_MAX);
		descriptor_write[binding_num].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptor_write[binding_num].pNext            = nullptr;
		descriptor_write[binding_num].dstSet           = new_set->set;
		descriptor_write[binding_num].dstBinding       = bind.gds_pointers.binding_index;
		descriptor_write[binding_num].dstArrayElement  = 0;
		descriptor_write[binding_num].descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		descriptor_write[binding_num].descriptorCount  = gds_buffers_num;
		descriptor_write[binding_num].pBufferInfo      = gds_buffer_info;
		descriptor_write[binding_num].pImageInfo       = nullptr;
		descriptor_write[binding_num].pTexelBufferView = nullptr;
		binding_num++;
	}

	vkUpdateDescriptorSets(gctx->device, binding_num, descriptor_write, 0, nullptr);

	Set nset;
	nset.set                    = new_set;
	nset.storage_buffers_num    = storage_buffers_num;
	nset.textures2d_sampled_num = textures2d_sampled_num;
	nset.textures2d_storage_num = textures2d_storage_num;
	nset.samplers_num           = samplers_num;
	nset.gds_buffers_num        = gds_buffers_num;
	nset.stage                  = stage;
	for (int i = 0; i < storage_buffers_num; i++)
	{
		nset.storage_buffers_id[i] = storage_buffers[i]->memory.unique_id;
	}
	for (int i = 0; i < textures2d_sampled_num; i++)
	{
		nset.textures2d_sampled_id[i] = textures2d_sampled[i]->memory.unique_id;
	}
	for (int i = 0; i < textures2d_storage_num; i++)
	{
		nset.textures2d_storage_id[i] = textures2d_storage[i]->memory.unique_id;
	}
	for (int i = 0; i < samplers_num; i++)
	{
		nset.samplers_id[i] = samplers[i];
	}
	for (int i = 0; i < gds_buffers_num; i++)
	{
		nset.gds_buffers_id[i] = gds_buffers[i]->memory.unique_id;
	}

	for (auto& set: m_sets)
	{
		if (set.set == nullptr)
		{
			set = nset;
			return new_set;
		}
	}

	m_sets.Add(nset);

	return new_set;
}

void DescriptorCache::FreeDescriptor(VulkanBuffer* buffer)
{
	EXIT_IF(buffer == nullptr);

	Core::LockGuard lock(m_mutex);

	for (auto& set: m_sets)
	{
		if (set.set != nullptr)
		{
			for (int i = 0; i < set.storage_buffers_num; i++)
			{
				if (set.storage_buffers_id[i] == buffer->memory.unique_id)
				{
					Free(set.set);
					set.set = nullptr;
					break;
				}
			}
		}
	}
}

void DescriptorCache::FreeDescriptor(VulkanImage* image)
{
	EXIT_IF(image == nullptr);

	Core::LockGuard lock(m_mutex);

	for (auto& set: m_sets)
	{
		if (set.set != nullptr)
		{
			for (int i = 0; i < set.textures2d_sampled_num; i++)
			{
				if (set.textures2d_sampled_id[i] == image->memory.unique_id)
				{
					Free(set.set);
					set.set = nullptr;
					break;
				}
			}
		}
	}
}

VkDescriptorSetLayout DescriptorCache::GetDescriptorSetLayout(Stage stage, const ShaderBindResources& bind,
                                                              const ShaderBindParameters& bind_params)
{
	int storage_buffers_num    = bind.storage_buffers.buffers_num;
	int textures2d_sampled_num = bind_params.textures2d_sampled_num;
	int textures2d_storage_num = bind_params.textures2d_storage_num;
	int samplers_num           = bind.samplers.samplers_num;
	int gds_buffers_num        = bind.gds_pointers.pointers_num;

	EXIT_IF(storage_buffers_num < 0 || storage_buffers_num > BUFFERS_MAX);
	EXIT_IF(textures2d_sampled_num < 0 || textures2d_sampled_num > TEXTURES_SAMPLED_MAX);
	EXIT_IF(textures2d_storage_num < 0 || textures2d_storage_num > TEXTURES_STORAGE_MAX);
	EXIT_IF(samplers_num < 0 || samplers_num > SAMPLERS_MAX);
	EXIT_IF(gds_buffers_num < 0 || gds_buffers_num > GDS_BUFFER_MAX);

	EXIT_NOT_IMPLEMENTED(stage != Stage::Pixel && stage != Stage::Compute &&
	                     (textures2d_sampled_num > 0 || textures2d_storage_num > 0 || samplers_num > 0));

	Core::LockGuard lock(m_mutex);
	Init();
	switch (stage)
	{
		case Stage::Vertex:
			return m_descriptor_set_layout_vertex[storage_buffers_num][textures2d_sampled_num][textures2d_storage_num][samplers_num]
			                                     [gds_buffers_num];
		case Stage::Pixel:
			return m_descriptor_set_layout_pixel[storage_buffers_num][textures2d_sampled_num][textures2d_storage_num][samplers_num]
			                                    [gds_buffers_num];
		case Stage::Compute:
			return m_descriptor_set_layout_compute[storage_buffers_num][textures2d_sampled_num][textures2d_storage_num][samplers_num]
			                                      [gds_buffers_num];
		default: EXIT("unknown stage\n");
	}
	return nullptr;
}

void DeleteFramebuffer(VideoOutVulkanImage* image)
{
	g_render_ctx->GetFramebufferCache()->FreeFramebufferByColor(image);
}

void DeleteFramebuffer(RenderTextureVulkanImage* image)
{
	g_render_ctx->GetFramebufferCache()->FreeFramebufferByColor(image);
}

void DeleteFramebuffer(DepthStencilVulkanImage* image)
{
	g_render_ctx->GetFramebufferCache()->FreeFramebufferByDepth(image);
}

void DeleteDescriptor(VulkanBuffer* buffer)
{
	g_render_ctx->GetDescriptorCache()->FreeDescriptor(buffer);
}

void DeleteDescriptor(TextureVulkanImage* image)
{
	g_render_ctx->GetDescriptorCache()->FreeDescriptor(image);
}

void DeleteDescriptor(StorageTextureVulkanImage* image)
{
	g_render_ctx->GetDescriptorCache()->FreeDescriptor(image);
}

void DeleteDescriptor(RenderTextureVulkanImage* image)
{
	g_render_ctx->GetDescriptorCache()->FreeDescriptor(image);
}

static bool get_stencil_op(VkStencilOp* f, uint32_t* ref, uint8_t op, uint8_t testval, uint8_t opval)
{
	VkStencilOp vk      = VK_STENCIL_OP_KEEP;
	uint32_t    r       = opval;
	bool        use_ref = true;

	switch (op)
	{
		case 0x00:
			vk      = VK_STENCIL_OP_KEEP;
			use_ref = false;
			break; /* Keep           */
		case 0x01:
			vk      = VK_STENCIL_OP_ZERO;
			use_ref = false;
			break; /* Zero           */
		case 0x02:
			vk = VK_STENCIL_OP_REPLACE;
			r  = 0xFF;
			break; /* Ones           */
		case 0x03:
			vk = VK_STENCIL_OP_REPLACE;
			r  = testval;
			break;                                                /* ReplaceTest    */
		case 0x04: vk = VK_STENCIL_OP_REPLACE; break;             /* ReplaceOp      */
		case 0x05: vk = VK_STENCIL_OP_INCREMENT_AND_CLAMP; break; /* AddClamp       */
		case 0x06: vk = VK_STENCIL_OP_DECREMENT_AND_CLAMP; break; /* SubClamp       */
		case 0x07: vk = VK_STENCIL_OP_INVERT; break;              /* Invert         */
		case 0x08: vk = VK_STENCIL_OP_INCREMENT_AND_WRAP; break;  /* AddWrap        */
		case 0x09: vk = VK_STENCIL_OP_DECREMENT_AND_WRAP; break;  /* SubWrap        */
		case 0x0a:                                                /* And            */
		case 0x0b:                                                /* Or             */
		case 0x0c:                                                /* Xor            */
		case 0x0d:                                                /* Nand           */
		case 0x0e:                                                /* Nor            */
		case 0x0f:                                                /* Xnor           */
		default: EXIT("invalid op");
	}
	*f   = vk;
	*ref = r;
	return use_ref;
}

static void get_stencil_state(PipelineStencilStaticState* s, PipelineStencilDynamicState* d, uint8_t func, uint8_t fail, uint8_t zpass,
                              uint8_t zfail, uint8_t testval, uint8_t mask, uint8_t writemask, uint8_t opval)
{
	EXIT_IF(s == nullptr || d == nullptr);

	uint32_t ref[3]     = {};
	bool     use_ref[3] = {};

	use_ref[0]     = get_stencil_op(&s->failOp, ref + 0, fail, testval, opval);
	use_ref[1]     = get_stencil_op(&s->passOp, ref + 1, zpass, testval, opval);
	use_ref[2]     = get_stencil_op(&s->depthFailOp, ref + 2, zfail, testval, opval);
	s->compareOp   = static_cast<VkCompareOp>(func);
	d->compareMask = mask;
	d->writeMask   = writemask;

	if (use_ref[0])
	{
		EXIT_NOT_IMPLEMENTED((ref[0] != ref[1] && use_ref[1]) || (ref[0] != ref[2] && use_ref[2]));
		d->reference = ref[0];
	} else if (use_ref[1])
	{
		EXIT_NOT_IMPLEMENTED(ref[1] != ref[2] && use_ref[2]);
		d->reference = ref[1];
	} else if (use_ref[2])
	{
		d->reference = ref[2];
	} else
	{
		d->reference = 0;
	}
}

static void FindRenderDepthInfo(CommandBuffer* /*buffer*/, const HardwareContext& hw, RenderDepthInfo* r)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(r == nullptr);

	bool        neo = Config::IsNeo();
	const auto& z   = hw.GetDepthRenderTarget();
	const auto& rc  = hw.GetRenderControl();
	const auto& dc  = hw.GetDepthControl();
	const auto& sc  = hw.GetStencilControl();
	const auto& sm  = hw.GetStencilMask();

	uint32_t size         = 0;
	uint32_t stencil_size = 0;
	uint32_t htile_size   = 0;
	uint32_t depth_size   = 0;
	uint32_t pitch        = 0;

	if (z.z_info.format == 3)
	{
		size = (z.slice_div64_minus1 + 1) * 64 * 4;
	} else if (z.z_info.format == 1)
	{
		size = (z.slice_div64_minus1 + 1) * 64 * 2;
	}

	bool htile = z.z_info.tile_surface_enable;

	TileGetDepthSize(z.width, z.height, z.z_info.format, z.stencil_info.format, htile, neo, &stencil_size, &htile_size, &depth_size,
	                 &pitch);

	if (pitch == 0)
	{
		depth_size = size;
	} else
	{
		EXIT_NOT_IMPLEMENTED(depth_size != size);
		EXIT_NOT_IMPLEMENTED((z.pitch_div8_minus1 + 1) * 8 != pitch);
	}

	switch (z.z_info.format * 2 + z.stencil_info.format)
	{
		case 0: r->format = VK_FORMAT_UNDEFINED; break;
		case 1: /*r->format = VK_FORMAT_S8_UINT*/ EXIT("Unsupported format: VK_FORMAT_S8_UINT\n"); break;
		case 2: r->format = VK_FORMAT_D16_UNORM; break;
		case 3: r->format = VK_FORMAT_D24_UNORM_S8_UINT; break;
		case 6: r->format = VK_FORMAT_D32_SFLOAT; break;
		case 7: r->format = VK_FORMAT_D32_SFLOAT_S8_UINT; break;
		default: EXIT("unknown z and stencil format: %u, %u\n", z.z_info.format, z.stencil_info.format);
	}

	r->htile                = htile;
	r->neo                  = neo;
	r->depth_buffer_size    = depth_size;
	r->depth_buffer_vaddr   = z.z_read_base_addr;
	r->stencil_buffer_size  = stencil_size;
	r->stencil_buffer_vaddr = z.stencil_read_base_addr;
	r->htile_buffer_size    = htile_size;
	r->htile_buffer_vaddr   = z.htile_data_base_addr;
	r->width                = z.width;
	r->height               = z.height;
	r->depth_clear_enable   = rc.depth_clear_enable;
	r->depth_clear_value    = hw.GetDepthClearValue();
	r->depth_test_enable    = dc.z_enable;
	r->depth_write_enable   = dc.z_write_enable;
	r->depth_compare_op     = static_cast<VkCompareOp>(dc.zfunc);

	r->depth_bounds_test_enable = dc.depth_bounds_enable;
	r->depth_min_bounds         = 0.0f;
	r->depth_max_bounds         = 0.0f;
	EXIT_NOT_IMPLEMENTED(r->depth_bounds_test_enable);

	r->stencil_clear_enable = rc.stencil_clear_enable;
	r->stencil_clear_value  = hw.GetStencilClearValue();
	// EXIT_NOT_IMPLEMENTED(r->stencil_clear_enable);

	r->stencil_test_enable = dc.stencil_enable;
	if (r->stencil_test_enable)
	{
		get_stencil_state(&r->stencil_static_front, &r->stencil_dynamic_front, dc.stencilfunc, sc.stencil_fail, sc.stencil_zpass,
		                  sc.stencil_zfail, sm.stencil_testval, sm.stencil_mask, sm.stencil_writemask, sm.stencil_opval);
		if (dc.backface_enable)
		{
			get_stencil_state(&r->stencil_static_back, &r->stencil_dynamic_back, dc.stencilfunc_bf, sc.stencil_fail_bf, sc.stencil_zpass_bf,
			                  sc.stencil_zfail_bf, sm.stencil_testval_bf, sm.stencil_mask_bf, sm.stencil_writemask_bf, sm.stencil_opval_bf);
		} else
		{
			r->stencil_static_back  = r->stencil_static_front;
			r->stencil_dynamic_back = r->stencil_dynamic_front;
		}
	} else
	{
		r->stencil_static_front  = {};
		r->stencil_static_back   = {};
		r->stencil_dynamic_front = {};
		r->stencil_dynamic_back  = {};
	}
	// EXIT_NOT_IMPLEMENTED(r->stencil_test_enable);
	EXIT_NOT_IMPLEMENTED(dc.backface_enable);

	r->vulkan_buffer = nullptr;

	if (r->format != VK_FORMAT_UNDEFINED)
	{
		DepthStencilBufferObject vulkan_buffer_info(r->format, r->width, r->height, htile, neo);

		r->vaddr_num = 0;

		if (r->depth_buffer_size > 0)
		{
			r->vaddr[r->vaddr_num] = r->depth_buffer_vaddr;
			r->size[r->vaddr_num]  = r->depth_buffer_size;
			r->vaddr_num++;
		}

		if (r->stencil_buffer_size > 0)
		{
			r->vaddr[r->vaddr_num] = r->stencil_buffer_vaddr;
			r->size[r->vaddr_num]  = r->stencil_buffer_size;
			r->vaddr_num++;
		}

		if (r->htile_buffer_size > 0)
		{
			r->vaddr[r->vaddr_num] = r->htile_buffer_vaddr;
			r->size[r->vaddr_num]  = r->htile_buffer_size;
			r->vaddr_num++;
		}

		EXIT_NOT_IMPLEMENTED(r->vaddr_num == 0);

		r->vulkan_buffer = static_cast<DepthStencilVulkanImage*>(
		    GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), nullptr, r->vaddr, r->size, r->vaddr_num, vulkan_buffer_info));

		EXIT_NOT_IMPLEMENTED(r->vulkan_buffer == nullptr);
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void FindRenderColorInfo(CommandBuffer* buffer, const HardwareContext& hw, RenderColorInfo* r)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(r == nullptr);

	const auto& rt   = hw.GetRenderTargets(0);
	auto        mask = hw.GetRenderTargetMask();

	if (rt.base_addr == 0 || mask == 0)
	{
		// No color output
		r->type          = RenderColorType::NoColorOutput;
		r->base_addr     = 0;
		r->vulkan_buffer = nullptr;
		r->buffer_size   = 0;
		return;
	}

	auto     width             = rt.width;
	auto     height            = rt.height;
	auto     pitch             = (rt.pitch_div8_minus1 + 1) * 8;
	auto     size              = (rt.slice_div64_minus1 + 1) * 64 * 4;
	bool     tile              = false;
	uint32_t format            = 0;
	bool     render_to_texture = false;

	if (rt.tile_mode == 0x8)
	{
		tile = false;
	} else if (rt.tile_mode == 0xa)
	{
		tile = true;
	} else if (rt.tile_mode == 0xd || rt.tile_mode == 0xe)
	{
		tile              = true;
		render_to_texture = true;
	} else
	{
		EXIT("unknown tile mode: %u\n", rt.tile_mode);
	}

	if (render_to_texture)
	{
		if (rt.color_info.format == 0xa && rt.color_info.channel_type == 0x0 && rt.color_info.channel_order == 0x0)
		{
			// Render to texture
			RenderTextureObject vulkan_buffer_info(RenderTextureFormat::R8G8B8A8Unorm, width, height, tile, rt.color_info.neo_mode, pitch);
			auto*               buffer_vulkan = static_cast<Graphics::RenderTextureVulkanImage*>(
                Graphics::GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), buffer, rt.base_addr, size, vulkan_buffer_info));
			EXIT_NOT_IMPLEMENTED(buffer_vulkan == nullptr);
			r->type          = RenderColorType::RenderTexture;
			r->base_addr     = rt.base_addr;
			r->vulkan_buffer = buffer_vulkan;
			r->buffer_size   = size;
		} else
		{
			EXIT("unknown format");
		}
	} else
	{
		if (rt.color_info.format == 0xa && rt.color_info.channel_type == 0x6 && rt.color_info.channel_order == 0x1)
		{
			format = 0x80000000;
		} else
		{
			EXIT("unknown format");
		}

		auto video_image = VideoOut::VideoOutGetImage(rt.base_addr);
		if (video_image.image == nullptr)
		{
			// Offscreen buffer
			VideoOutBufferObject vulkan_buffer_info(format, width, height, tile, Config::IsNeo(), pitch);
			auto*                buffer_vulkan = static_cast<Graphics::VideoOutVulkanImage*>(
                Graphics::GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), nullptr, rt.base_addr, size, vulkan_buffer_info));
			EXIT_NOT_IMPLEMENTED(buffer_vulkan == nullptr);
			r->type          = RenderColorType::OffscreenBuffer;
			r->base_addr     = rt.base_addr;
			r->vulkan_buffer = buffer_vulkan;
			r->buffer_size   = size;
		} else
		{
			// Display buffer
			EXIT_NOT_IMPLEMENTED(video_image.buffer_size != size);
			EXIT_NOT_IMPLEMENTED(video_image.buffer_pitch != pitch);
			r->type          = RenderColorType::DisplayBuffer;
			r->base_addr     = rt.base_addr;
			r->vulkan_buffer = video_image.image;
			r->buffer_size   = video_image.buffer_size;
		}
	}
}

static void InvalidateMemoryObject(const RenderColorInfo& r)
{
	bool with_color = (r.vulkan_buffer != nullptr);

	if (with_color)
	{
		if (r.type == RenderColorType::RenderTexture)
		{
			GpuMemoryResetHash(g_render_ctx->GetGraphicCtx(), &r.base_addr, &r.buffer_size, 1, GpuMemoryObjectType::RenderTexture);
		} else if (r.type == RenderColorType::DisplayBuffer || r.type == RenderColorType::OffscreenBuffer)
		{
			GpuMemoryResetHash(g_render_ctx->GetGraphicCtx(), &r.base_addr, &r.buffer_size, 1, GpuMemoryObjectType::VideoOutBuffer);
		}
	}
}

static void InvalidateMemoryObject(const RenderDepthInfo& r)
{
	bool with_depth = (r.format != VK_FORMAT_UNDEFINED && r.vulkan_buffer != nullptr);

	if (with_depth)
	{
		GpuMemoryResetHash(g_render_ctx->GetGraphicCtx(), r.vaddr, r.size, r.vaddr_num, GpuMemoryObjectType::DepthStencilBuffer);
	}
}

static Vector<RenderTextureVulkanImage*> FindRenderTexture(uint64_t vaddr, uint64_t size, bool exact)
{
	KYTY_PROFILER_FUNCTION();

	Vector<RenderTextureVulkanImage*> ret;

	auto objects = GpuMemoryFindObjects(vaddr, size, exact);

	for (const auto& obj: objects)
	{
		if (obj.type == GpuMemoryObjectType::RenderTexture)
		{
			ret.Add(static_cast<RenderTextureVulkanImage*>(obj.obj));
		}
	}

	return ret;
}

static void PrepareStorageBuffers(CommandBuffer* buffer, const ShaderStorageResources& storage_buffers, VulkanBuffer** buffers,
                                  uint32_t** sgprs)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(buffers == nullptr);
	EXIT_IF(sgprs == nullptr);
	EXIT_IF(*sgprs == nullptr);

	for (int i = 0; i < storage_buffers.buffers_num; i++)
	{
		auto r = storage_buffers.buffers[i];

		EXIT_NOT_IMPLEMENTED(r.AddTid());
		EXIT_NOT_IMPLEMENTED(r.SwizzleEnabled());
		EXIT_NOT_IMPLEMENTED(!((r.Stride() == 4 && r.DstSelX() == 4 && r.DstSelY() == 0 && r.DstSelZ() == 0 && r.DstSelW() == 0 &&
		                        r.Dfmt() == 4 && r.Nfmt() == 4) ||
		                       (r.Stride() == 4 && r.DstSelX() == 4 && r.DstSelY() == 0 && r.DstSelZ() == 0 && r.DstSelW() == 1 &&
		                        r.Dfmt() == 4 && r.Nfmt() == 7) ||
		                       (r.Stride() == 8 && r.DstSelX() == 4 && r.DstSelY() == 5 && r.DstSelZ() == 0 && r.DstSelW() == 0 &&
		                        r.Dfmt() == 11 && r.Nfmt() == 4) ||
		                       (r.Stride() == 16 && r.DstSelX() == 4 && r.DstSelY() == 5 && r.DstSelZ() == 6 && r.DstSelW() == 7 &&
		                        r.Dfmt() == 14 && r.Nfmt() == 7)));
		EXIT_NOT_IMPLEMENTED(!(r.MemoryType() == 0x00 || r.MemoryType() == 0x10 || r.MemoryType() == 0x6d));

		auto addr        = r.Base();
		auto stride      = r.Stride();
		auto num_records = r.NumRecords();
		auto size        = stride * num_records;
		EXIT_NOT_IMPLEMENTED(size == 0);
		EXIT_NOT_IMPLEMENTED((size & 0x3u) != 0);

		bool read_only = (r.MemoryType() == 0x10);

		EXIT_NOT_IMPLEMENTED(read_only && !(storage_buffers.usages[i] == ShaderStorageUsage::ReadOnly ||
		                                    storage_buffers.usages[i] == ShaderStorageUsage::Constant));

		StorageBufferGpuObject buf_info(stride, num_records, read_only);

		auto* buf = static_cast<StorageVulkanBuffer*>(GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), nullptr, addr, size, buf_info));

		EXIT_NOT_IMPLEMENTED(buf == nullptr);

		StorageBufferSet(buffer, buf);

		buffers[i] = buf;

		r.UpdateAddress(i);

		EXIT_NOT_IMPLEMENTED((r.Base() >> 32u) != 0);

		(*sgprs)[0] = r.fields[0];
		(*sgprs)[1] = r.fields[1];
		(*sgprs)[2] = r.fields[2];
		(*sgprs)[3] = r.fields[3];

		(*sgprs) += 4;
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void PrepareTextures(CommandBuffer* buffer, const ShaderTextureResources& textures, VulkanImage** images_sampled,
                            VulkanImage** images_storage, uint32_t** sgprs, const bool* without_sampler)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(images_sampled == nullptr);
	EXIT_IF(images_storage == nullptr);
	EXIT_IF(sgprs == nullptr);
	EXIT_IF(*sgprs == nullptr);
	EXIT_IF(without_sampler == nullptr);

	int index_sampled = 0;
	int index_storage = 0;

	for (int i = 0; i < textures.textures_num; i++)
	{
		auto r = textures.textures[i];

		EXIT_NOT_IMPLEMENTED(r.Base() == 0);
		EXIT_NOT_IMPLEMENTED(r.MinLod() != 0);
		EXIT_NOT_IMPLEMENTED(r.Dfmt() != 10 && r.Dfmt() != 37);
		EXIT_NOT_IMPLEMENTED(r.Nfmt() != 9 && r.Nfmt() != 0);
		// EXIT_NOT_IMPLEMENTED(r.Width() != 511);
		// EXIT_NOT_IMPLEMENTED(r.Height() != 511);
		EXIT_NOT_IMPLEMENTED(r.PerfMod() != 7 && r.PerfMod() != 0);
		EXIT_NOT_IMPLEMENTED(r.Interlaced() != false);
		// EXIT_NOT_IMPLEMENTED(r.DstSelX() != 4);
		// EXIT_NOT_IMPLEMENTED(r.DstSelY() != 5);
		// EXIT_NOT_IMPLEMENTED(r.DstSelZ() != 6);
		// EXIT_NOT_IMPLEMENTED(r.DstSelW() != 7);
		// EXIT_NOT_IMPLEMENTED(r.BaseLevel() != 0);
		// EXIT_NOT_IMPLEMENTED(r.LastLevel() != 0 && r.LastLevel() != 9);
		EXIT_NOT_IMPLEMENTED(!(r.TilingIdx() == 8 || r.TilingIdx() == 13 || r.TilingIdx() == 14));
		// EXIT_NOT_IMPLEMENTED(!(r.LastLevel() == 0 && r.Pow2Pad() == false) && !(r.LastLevel() == 9 && r.Pow2Pad() == true));
		EXIT_NOT_IMPLEMENTED(r.Type() != 9);
		EXIT_NOT_IMPLEMENTED(r.Depth() != 0);
		// EXIT_NOT_IMPLEMENTED(r.Pitch() != 511);
		EXIT_NOT_IMPLEMENTED(r.BaseArray() != 0);
		EXIT_NOT_IMPLEMENTED(r.LastArray() != 0);
		EXIT_NOT_IMPLEMENTED(r.MinLodWarn() != 0);
		EXIT_NOT_IMPLEMENTED(r.CounterBankId() != 0);
		EXIT_NOT_IMPLEMENTED(r.LodHdwCntEn() != false);
		EXIT_NOT_IMPLEMENTED(r.MemoryType() != 0x10 && r.MemoryType() != 0x6d);

		bool read_only = (r.MemoryType() == 0x10);

		EXIT_NOT_IMPLEMENTED(read_only && !(textures.usages[i] == ShaderTextureUsage::ReadOnly));
		// EXIT_NOT_IMPLEMENTED(!read_only && !(textures.usages[i] == ShaderTextureUsage::ReadWrite));

		auto     addr       = r.Base();
		uint32_t size       = 0;
		bool     neo        = Config::IsNeo();
		auto     width      = r.Width() + 1;
		auto     height     = r.Height() + 1;
		auto     pitch      = r.Pitch() + 1;
		auto     base_level = r.BaseLevel();
		auto     levels     = r.LastLevel() + 1;
		auto     tile       = r.TilingIdx();
		uint32_t swizzle    = static_cast<uint32_t>(r.DstSelX()) | (static_cast<uint32_t>(r.DstSelY()) << 8u) |
		                   (static_cast<uint32_t>(r.DstSelZ()) << 16u) | (static_cast<uint32_t>(r.DstSelW()) << 24u);

		TileGetTextureSize(r.Dfmt(), r.Nfmt(), width, height, pitch, levels, tile, neo, &size, nullptr, nullptr, nullptr);
		EXIT_NOT_IMPLEMENTED(size == 0);

		auto rtex = FindRenderTexture(addr, size, true);

		EXIT_NOT_IMPLEMENTED(rtex.Size() > 1);

		VulkanImage* tex = (!rtex.IsEmpty() ? rtex.At(0) : nullptr);

		if (tex == nullptr)
		{
			if (without_sampler[i])
			{
				StorageTextureObject vulkan_texture_info(r.Dfmt(), r.Nfmt(), width, height, pitch, base_level, levels, tile, neo, swizzle);

				tex = static_cast<StorageTextureVulkanImage*>(
				    GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), buffer, addr, size, vulkan_texture_info));
			} else
			{
				TextureObject vulkan_texture_info(r.Dfmt(), r.Nfmt(), width, height, pitch, base_level, levels, tile, neo, swizzle);

				tex = static_cast<TextureVulkanImage*>(
				    GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), buffer, addr, size, vulkan_texture_info));
			}
		}

		EXIT_NOT_IMPLEMENTED(tex == nullptr);

		if (without_sampler[i])
		{
			images_storage[index_storage] = tex;
			r.UpdateAddress(index_storage);
			index_storage++;
		} else
		{
			images_sampled[index_sampled] = tex;
			r.UpdateAddress(index_sampled);
			index_sampled++;
		}

		EXIT_NOT_IMPLEMENTED((r.Base() >> 32u) != 0);

		(*sgprs)[0] = r.fields[0];
		(*sgprs)[1] = r.fields[1];
		(*sgprs)[2] = r.fields[2];
		(*sgprs)[3] = r.fields[3];
		(*sgprs)[4] = r.fields[4];
		(*sgprs)[5] = r.fields[5];
		(*sgprs)[6] = r.fields[6];
		(*sgprs)[7] = r.fields[7];

		(*sgprs) += 8;
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void PrepareSamplers(const ShaderSamplerResources& samplers, uint64_t* sampler_ids, uint32_t** sgprs)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(sampler_ids == nullptr);
	EXIT_IF(sgprs == nullptr);
	EXIT_IF(*sgprs == nullptr);

	for (int i = 0; i < samplers.samplers_num; i++)
	{
		auto r = samplers.samplers[i];

		EXIT_NOT_IMPLEMENTED(r.ClampX() != 0);
		EXIT_NOT_IMPLEMENTED(r.ClampY() != 0);
		EXIT_NOT_IMPLEMENTED(r.ClampZ() != 0);
		// EXIT_NOT_IMPLEMENTED(r.MaxAnisoRatio() != 0);
		EXIT_NOT_IMPLEMENTED(r.DepthCompareFunc() != 0);
		EXIT_NOT_IMPLEMENTED(r.ForceUnormCoords() != false);
		EXIT_NOT_IMPLEMENTED(r.AnisoThreshold() != 0);
		EXIT_NOT_IMPLEMENTED(r.McCoordTrunc() != false);
		EXIT_NOT_IMPLEMENTED(r.ForceDegamma() != false);
		EXIT_NOT_IMPLEMENTED(r.AnisoBias() != 0);
		EXIT_NOT_IMPLEMENTED(r.TruncCoord() != false);
		EXIT_NOT_IMPLEMENTED(r.DisableCubeWrap() != false);
		EXIT_NOT_IMPLEMENTED(r.FilterMode() != 0);
		// EXIT_NOT_IMPLEMENTED(r.MinLod() != 0);
		// EXIT_NOT_IMPLEMENTED(r.MaxLod() != 4095);
		EXIT_NOT_IMPLEMENTED(r.PerfMip() != 0);
		EXIT_NOT_IMPLEMENTED(r.PerfZ() != 0);
		EXIT_NOT_IMPLEMENTED(r.LodBias() != 0);
		EXIT_NOT_IMPLEMENTED(r.LodBiasSec() != 0);
		// EXIT_NOT_IMPLEMENTED(r.XyMagFilter() != 1);
		// EXIT_NOT_IMPLEMENTED(r.XyMinFilter() != 1);
		EXIT_NOT_IMPLEMENTED(r.ZFilter() != 1);
		// EXIT_NOT_IMPLEMENTED(r.MipFilter() != 0 && r.MipFilter() != 2);
		EXIT_NOT_IMPLEMENTED(r.BorderColorPtr() != 0);
		EXIT_NOT_IMPLEMENTED(r.BorderColorType() != 0);

		sampler_ids[i] = g_render_ctx->GetSamplerCache()->GetSamplerId(r);

		r.UpdateIndex(i);

		(*sgprs)[0] = r.fields[0];
		(*sgprs)[1] = r.fields[1];
		(*sgprs)[2] = r.fields[2];
		(*sgprs)[3] = r.fields[3];

		(*sgprs) += 4;
	}
}

static void PrepareGdsPointers(const ShaderGdsResources& gds_pointers, uint32_t** sgprs)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(sgprs == nullptr);
	EXIT_IF(*sgprs == nullptr);

	for (int i = 0; i < gds_pointers.pointers_num; i++)
	{
		auto r = gds_pointers.pointers[i];

		EXIT_NOT_IMPLEMENTED(r.Size() != 4);

		(*sgprs)[i] = r.field;
	}

	if (gds_pointers.pointers_num > 0)
	{
		(*sgprs) += 4 * ((gds_pointers.pointers_num - 1) / 4 + 1);
	}
}

static void BindDescriptors(CommandBuffer* buffer, VkPipelineBindPoint pipeline_bind_point, VkPipelineLayout layout,
                            const ShaderBindResources& bind, const ShaderBindParameters& bind_params, VkShaderStageFlags vk_stage,
                            DescriptorCache::Stage stage)
{
	KYTY_PROFILER_FUNCTION();

	if (bind.push_constant_size > 0)
	{
		EXIT_NOT_IMPLEMENTED(bind.push_constant_size > DescriptorCache::PUSH_CONSTANTS_MAX * 4);
		EXIT_NOT_IMPLEMENTED(bind.storage_buffers.buffers_num > DescriptorCache::BUFFERS_MAX);
		EXIT_NOT_IMPLEMENTED((bind_params.textures2d_storage_num > DescriptorCache::TEXTURES_STORAGE_MAX) ||
		                     (bind_params.textures2d_sampled_num > DescriptorCache::TEXTURES_SAMPLED_MAX));
		EXIT_NOT_IMPLEMENTED(bind_params.textures2d_storage_num + bind_params.textures2d_sampled_num != bind.textures2D.textures_num);
		EXIT_NOT_IMPLEMENTED(bind.samplers.samplers_num > DescriptorCache::SAMPLERS_MAX);

		VulkanBuffer* storage_buffers[DescriptorCache::BUFFERS_MAX];
		VulkanImage*  textures2d_sampled[DescriptorCache::TEXTURES_SAMPLED_MAX];
		VulkanImage*  textures2d_storage[DescriptorCache::TEXTURES_STORAGE_MAX];
		uint64_t      samplers[DescriptorCache::SAMPLERS_MAX];
		uint32_t      sgprs[DescriptorCache::PUSH_CONSTANTS_MAX];

		uint32_t* sgprs_ptr = sgprs;

		VulkanBuffer* gds_buffer = nullptr;

		if (bind.storage_buffers.buffers_num > 0)
		{
			PrepareStorageBuffers(buffer, bind.storage_buffers, storage_buffers, &sgprs_ptr);
		}
		if (bind.textures2D.textures_num > 0)
		{
			PrepareTextures(buffer, bind.textures2D, textures2d_sampled, textures2d_storage, &sgprs_ptr,
			                bind_params.textures2d_without_sampler);
		}
		if (bind.samplers.samplers_num > 0)
		{
			PrepareSamplers(bind.samplers, samplers, &sgprs_ptr);
		}
		if (bind.gds_pointers.pointers_num > 0)
		{
			PrepareGdsPointers(bind.gds_pointers, &sgprs_ptr);
			gds_buffer = g_render_ctx->GetGdsBuffer()->GetBuffer(g_render_ctx->GetGraphicCtx());
		}

		EXIT_IF(bind.push_constant_size != (sgprs_ptr - sgprs) * 4);

		auto* descriptor_set = g_render_ctx->GetDescriptorCache()->GetDescriptor(
		    stage, storage_buffers, textures2d_sampled, textures2d_storage, samplers, &gds_buffer, bind, bind_params);

		EXIT_IF(descriptor_set == nullptr);

		auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

		vkCmdBindDescriptorSets(vk_buffer, pipeline_bind_point, layout, bind.descriptor_set_slot, 1, &descriptor_set->set, 0, nullptr);
		vkCmdPushConstants(vk_buffer, layout, vk_stage, bind.push_constant_offset, bind.push_constant_size, sgprs);
	}
}

static void SetDynamicParams(VkCommandBuffer vk_buffer, VulkanPipeline* pipeline)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(pipeline == nullptr);
	EXIT_IF(pipeline->static_params == nullptr);
	EXIT_IF(pipeline->dynamic_params == nullptr);

	if (pipeline->static_params->stencil_test_enable)
	{
		if (pipeline->dynamic_params->vk_dynamic_state_stencil_compare_mask)
		{
			vkCmdSetStencilCompareMask(vk_buffer, VK_STENCIL_FACE_FRONT_BIT, pipeline->dynamic_params->stencil_front.compareMask);
			vkCmdSetStencilCompareMask(vk_buffer, VK_STENCIL_FACE_BACK_BIT, pipeline->dynamic_params->stencil_back.compareMask);
		}
		if (pipeline->dynamic_params->vk_dynamic_state_stencil_write_mask)
		{
			vkCmdSetStencilWriteMask(vk_buffer, VK_STENCIL_FACE_FRONT_BIT, pipeline->dynamic_params->stencil_front.writeMask);
			vkCmdSetStencilWriteMask(vk_buffer, VK_STENCIL_FACE_BACK_BIT, pipeline->dynamic_params->stencil_back.writeMask);
		}
		if (pipeline->dynamic_params->vk_dynamic_state_stencil_reference)
		{
			vkCmdSetStencilReference(vk_buffer, VK_STENCIL_FACE_FRONT_BIT, pipeline->dynamic_params->stencil_front.reference);
			vkCmdSetStencilReference(vk_buffer, VK_STENCIL_FACE_BACK_BIT, pipeline->dynamic_params->stencil_back.reference);
		}
	}
}

void GraphicsRenderDrawIndex(CommandBuffer* buffer, HardwareContext* ctx, UserConfig* ucfg, uint32_t index_type_and_size,
                             uint32_t index_count, const void* index_addr, uint32_t flags, uint32_t type)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(ucfg == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	if (const auto& vs = ctx->GetVs(); !vs.vs_embedded && ShaderIsDisabled(vs.vs_regs.GetGpuAddress()))
	{
		return;
	}

	if (const auto& ps = ctx->GetPs(); !ps.ps_embedded && ShaderIsDisabled(ps.ps_regs.data_addr))
	{
		return;
	}

	// const auto& rt = ctx->GetRenderTargets(0);
	const auto& bc = ctx->GetBlendControl(0);

	uc_print("GraphicsRenderDrawIndex():UserConfig:", *ucfg);

	hw_print(*ctx);
	hw_check(*ctx);

	EXIT_NOT_IMPLEMENTED(ucfg->GetPrimType() != 4);
	EXIT_NOT_IMPLEMENTED(ctx->GetShaderStages() != 0);

	printf("GraphicsRenderDrawIndex():Parameters:\n");
	printf("\t index_type_and_size = 0x%08" PRIx32 "\n", index_type_and_size);
	printf("\t index_count         = 0x%08" PRIx32 "\n", index_count);
	printf("\t index_addr          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(index_addr));
	printf("\t flags               = 0x%08" PRIx32 "\n", flags);
	printf("\t type                = 0x%08" PRIx32 "\n", type);

	VkIndexType index_type = VK_INDEX_TYPE_UINT16;
	uint64_t    index_size = 0;

	switch (index_type_and_size)
	{
		case 0:
			index_type = VK_INDEX_TYPE_UINT16;
			index_size = 2 * index_count;
			break;
		case 1:
			index_type = VK_INDEX_TYPE_UINT32;
			index_size = 4 * index_count;
			break;
		default: EXIT("unknown index_type_and_size: %u\n", index_type_and_size);
	}

	EXIT_NOT_IMPLEMENTED(flags != 0);
	EXIT_NOT_IMPLEMENTED(type != 1);

	RenderDepthInfo depth_info;
	FindRenderDepthInfo(buffer, *ctx, &depth_info);

	RenderColorInfo color_info;
	FindRenderColorInfo(buffer, *ctx, &color_info);

	auto* framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(&color_info, &depth_info);

	EXIT_NOT_IMPLEMENTED(framebuffer == nullptr);
	EXIT_NOT_IMPLEMENTED(framebuffer->render_pass == nullptr);

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	ShaderVertexInputInfo vs_input_info;
	ShaderGetInputInfoVS(&ctx->GetVs(), &vs_input_info);

	ShaderPixelInputInfo ps_input_info;
	ShaderGetInputInfoPS(&ctx->GetPs(), &vs_input_info, &ps_input_info);

	EXIT_NOT_IMPLEMENTED(vs_input_info.buffers_num > 1);

	auto* pipeline = g_render_ctx->GetPipelineCache()->CreatePipeline(framebuffer, &color_info, &depth_info, &vs_input_info, &ctx->GetVs(),
	                                                                  &ps_input_info, &ctx->GetPs(), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	                                                                  ctx->GetRenderTargetMask(), ctx->GetModeControl(), bc,
	                                                                  ctx->GetBlendColor(), ctx->GetScreenViewport());

	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

	SetDynamicParams(vk_buffer, pipeline);

	for (int i = 0; i < vs_input_info.buffers_num; i++)
	{
		const auto& b    = vs_input_info.buffers[i];
		uint64_t    addr = b.addr;
		uint64_t    size = b.stride * b.num_records;

		auto* vertices =
		    static_cast<VulkanBuffer*>(GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), nullptr, addr, size, VertexBufferGpuObject()));

		VkDeviceSize offset = 0;

		vkCmdBindVertexBuffers(vk_buffer, i, 1, &vertices->buffer, &offset);
	}

	BindDescriptors(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, vs_input_info.bind,
	                pipeline->additional_params->vs_bind, VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex);

	BindDescriptors(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, ps_input_info.bind,
	                pipeline->additional_params->ps_bind, VK_SHADER_STAGE_FRAGMENT_BIT, DescriptorCache::Stage::Pixel);

	VulkanBuffer* indices = static_cast<VulkanBuffer*>(GpuMemoryCreateObject(
	    g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(index_addr), index_size, IndexBufferGpuObject()));

	EXIT_NOT_IMPLEMENTED(indices == nullptr);

	vkCmdBindIndexBuffer(vk_buffer, indices->buffer, 0, index_type);

	buffer->BeginRenderPass(framebuffer, &color_info, &depth_info);

	vkCmdDrawIndexed(vk_buffer, index_count, 1, 0, 0, 0);

	buffer->EndRenderPass();

	InvalidateMemoryObject(color_info);
	InvalidateMemoryObject(depth_info);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void GraphicsRenderDrawIndexAuto(CommandBuffer* buffer, HardwareContext* ctx, UserConfig* ucfg, uint32_t index_count, uint32_t flags)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr || ucfg == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr || buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	if (const auto& vs = ctx->GetVs(); !vs.vs_embedded && ShaderIsDisabled(vs.vs_regs.GetGpuAddress()))
	{
		return;
	}

	if (const auto& ps = ctx->GetPs(); !ps.ps_embedded && ShaderIsDisabled(ps.ps_regs.data_addr))
	{
		return;
	}

	// const auto& rt = ctx->GetRenderTargets(0);
	const auto& bc = ctx->GetBlendControl(0);

	uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", *ucfg);

	hw_print(*ctx);
	hw_check(*ctx);

	printf("GraphicsRenderDrawIndex():Parameters:\n");
	printf("\t index_count         = 0x%08" PRIx32 "\n", index_count);
	printf("\t flags               = 0x%08" PRIx32 "\n", flags);

	EXIT_NOT_IMPLEMENTED(flags != 0);
	EXIT_NOT_IMPLEMENTED(ctx->GetShaderStages() != 0);

	RenderDepthInfo depth_info;
	FindRenderDepthInfo(buffer, *ctx, &depth_info);

	RenderColorInfo color_info;
	FindRenderColorInfo(buffer, *ctx, &color_info);

	auto* framebuffer = g_render_ctx->GetFramebufferCache()->CreateFramebuffer(&color_info, &depth_info);

	EXIT_NOT_IMPLEMENTED(framebuffer == nullptr);
	EXIT_NOT_IMPLEMENTED(framebuffer->render_pass == nullptr);

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

	switch (ucfg->GetPrimType())
	{
		case 4: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
		case 17: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
		case 19: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
		default: EXIT("unknown primitive type: %u\n", ucfg->GetPrimType());
	}

	const auto& vertex_shader_info = ctx->GetVs();
	const auto& pixel_shader_info  = ctx->GetPs();

	ShaderVertexInputInfo vs_input_info;
	ShaderGetInputInfoVS(&vertex_shader_info, &vs_input_info);

	ShaderPixelInputInfo ps_input_info;
	ShaderGetInputInfoPS(&pixel_shader_info, &vs_input_info, &ps_input_info);

	EXIT_NOT_IMPLEMENTED(vs_input_info.buffers_num > 1);

	auto* pipeline = g_render_ctx->GetPipelineCache()->CreatePipeline(
	    framebuffer, &color_info, &depth_info, &vs_input_info, &vertex_shader_info, &ps_input_info, &pixel_shader_info, topology,
	    ctx->GetRenderTargetMask(), ctx->GetModeControl(), bc, ctx->GetBlendColor(), ctx->GetScreenViewport());

	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);

	SetDynamicParams(vk_buffer, pipeline);

	for (int i = 0; i < vs_input_info.buffers_num; i++)
	{
		const auto& b    = vs_input_info.buffers[i];
		uint64_t    addr = b.addr;
		uint64_t    size = b.stride * b.num_records;

		auto* vertices =
		    static_cast<VulkanBuffer*>(GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), nullptr, addr, size, VertexBufferGpuObject()));

		VkDeviceSize offset = 0;

		vkCmdBindVertexBuffers(vk_buffer, i, 1, &vertices->buffer, &offset);
	}

	BindDescriptors(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, vs_input_info.bind,
	                pipeline->additional_params->vs_bind, VK_SHADER_STAGE_VERTEX_BIT, DescriptorCache::Stage::Vertex);

	BindDescriptors(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline_layout, ps_input_info.bind,
	                pipeline->additional_params->ps_bind, VK_SHADER_STAGE_FRAGMENT_BIT, DescriptorCache::Stage::Pixel);

	buffer->BeginRenderPass(framebuffer, &color_info, &depth_info);

	switch (ucfg->GetPrimType())
	{
		case 4: vkCmdDraw(vk_buffer, index_count, 1, 0, 0); break;
		case 17:
			EXIT_NOT_IMPLEMENTED(!(/*vertex_shader_info.vs_embedded && vertex_shader_info.vs_embedded_id == 0 &&*/
			                       index_count == 3 && vs_input_info.buffers_num == 0));
			vkCmdDraw(vk_buffer, 4, 1, 0, 0);
			break;
		case 19:
			EXIT_NOT_IMPLEMENTED(index_count != 4);
			vkCmdDraw(vk_buffer, 4, 1, 0, 0);
			break;
		default: EXIT("unknown primitive type: %u\n", ucfg->GetPrimType());
	}

	buffer->EndRenderPass();

	InvalidateMemoryObject(color_info);
	InvalidateMemoryObject(depth_info);
}

void GraphicsRenderDispatchDirect(CommandBuffer* buffer, HardwareContext* ctx, uint32_t thread_group_x, uint32_t thread_group_y,
                                  uint32_t thread_group_z, uint32_t mode)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	if (ShaderIsDisabled(ctx->GetCs().cs_regs.data_addr))
	{
		return;
	}

	EXIT_NOT_IMPLEMENTED(mode != 0);

	const auto& cs_regs = ctx->GetCs();

	ShaderComputeInputInfo input_info;
	ShaderGetInputInfoCS(&cs_regs, &input_info);

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	auto* pipeline = g_render_ctx->GetPipelineCache()->CreatePipeline(&input_info, &ctx->GetCs());

	vkCmdBindPipeline(vk_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);

	SetDynamicParams(vk_buffer, pipeline);

	BindDescriptors(buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline_layout, input_info.bind,
	                pipeline->additional_params->cs_bind, VK_SHADER_STAGE_COMPUTE_BIT, DescriptorCache::Stage::Compute);

	vkCmdDispatch(vk_buffer, thread_group_x, thread_group_y, thread_group_z);
}

void GraphicsRenderMemoryBarrier(CommandBuffer* buffer)
{
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	VkMemoryBarrier mem_barrier {};
	mem_barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	mem_barrier.pNext         = nullptr;
	mem_barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	mem_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mem_barrier, 0, nullptr, 0,
	                     nullptr);
}

void GraphicsRenderRenderTextureBarrier(CommandBuffer* buffer, uint64_t vaddr, uint64_t size)
{
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	auto* vk_buffer = buffer->GetPool()->buffers[buffer->GetIndex()];

	auto images = FindRenderTexture(vaddr, size, false);

	for (auto* image: images)
	{
		EXIT_NOT_IMPLEMENTED(image == nullptr);
		if (image->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		{
			VkImageMemoryBarrier image_memory_barrier {};
			image_memory_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			image_memory_barrier.pNext                           = nullptr;
			image_memory_barrier.srcAccessMask                   = VK_ACCESS_MEMORY_WRITE_BIT;
			image_memory_barrier.dstAccessMask                   = VK_ACCESS_MEMORY_READ_BIT;
			image_memory_barrier.oldLayout                       = image->layout;
			image_memory_barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
			image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
			image_memory_barrier.image                           = image->image;
			image_memory_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
			image_memory_barrier.subresourceRange.baseMipLevel   = 0;
			image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
			image_memory_barrier.subresourceRange.baseArrayLayer = 0;
			image_memory_barrier.subresourceRange.layerCount     = 1;

			vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			                     VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
			                     &image_memory_barrier);

			image->layout = image_memory_barrier.newLayout;
		}
	}
}

void GraphicsRenderWriteAtEndOfPipe(CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	Graphics::LabelGpuObject label_info(value, nullptr, nullptr);

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 4, label_info));

	LabelSet(buffer, label);
}

void GraphicsRenderWriteAtEndOfPipeGds(CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t dw_offset, uint32_t dw_num)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	uint64_t args[4] = {static_cast<uint64_t>(dw_offset), static_cast<uint64_t>(dw_num), reinterpret_cast<uint64_t>(dst_gpu_addr), 0};

	Graphics::LabelGpuObject label_info(
	    0,
	    [](const uint64_t* args)
	    {
		    auto  dw_offset    = static_cast<uint32_t>(args[0]);
		    auto  dw_num       = static_cast<uint32_t>(args[1]);
		    auto* dst_gpu_addr = reinterpret_cast<uint32_t*>(args[2]);
		    g_render_ctx->GetGdsBuffer()->Read(g_render_ctx->GetGraphicCtx(), dst_gpu_addr, dw_offset, dw_num);
		    return false;
	    },
	    nullptr, args);

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 4, label_info));

	LabelSet(buffer, label);
}

void GraphicsRenderWriteAtEndOfPipe(CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	Graphics::LabelGpuObject label_info(value, nullptr, nullptr);

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info));

	LabelSet(buffer, label);
}

void GraphicsRenderWriteAtEndOfPipeClockCounter(CommandBuffer* buffer, uint64_t* dst_gpu_addr)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	uint64_t args[4] = {reinterpret_cast<uint64_t>(dst_gpu_addr), 0, 0, 0};

	Graphics::LabelGpuObject label_info(
	    0,
	    [](const uint64_t* args)
	    {
		    auto* dst_gpu_addr = reinterpret_cast<uint64_t*>(args[0]);
		    EXIT_IF(dst_gpu_addr == nullptr);
		    *dst_gpu_addr = LibKernel::KernelReadTsc();
		    printf(FG_BRIGHT_GREEN "EndOfPipe Signal!!! [0x%016" PRIx64 "] <- Clock: 0x%016" PRIx64 "\n" FG_DEFAULT,
		           reinterpret_cast<uint64_t>(dst_gpu_addr), *dst_gpu_addr);
		    return false;
	    },
	    nullptr, args);

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info));

	LabelSet(buffer, label);
}

void GraphicsRenderWriteAtEndOfPipeWithWriteBack(CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	Graphics::LabelGpuObject label_info(
	    value,
	    [](const uint64_t* /*args*/)
	    {
		    EXIT_IF(g_render_ctx == nullptr);
		    GpuMemoryWriteBack(g_render_ctx->GetGraphicCtx());
		    return true;
	    },
	    nullptr);

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info));

	LabelSet(buffer, label);
}

void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBack(CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	Graphics::LabelGpuObject label_info(
	    value,
	    [](const uint64_t* /*args*/)
	    {
		    EXIT_IF(g_render_ctx == nullptr);
		    GpuMemoryWriteBack(g_render_ctx->GetGraphicCtx());
		    return true;
	    },
	    [](const uint64_t* /*args*/)
	    {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent();
		    return true;
	    });

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info));

	LabelSet(buffer, label);
}

void GraphicsRenderWriteAtEndOfPipeWithInterrupt(CommandBuffer* buffer, uint64_t* dst_gpu_addr, uint64_t value)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	Graphics::LabelGpuObject label_info(value, nullptr,
	                                    [](const uint64_t* /*args*/)
	                                    {
		                                    EXIT_IF(g_render_ctx == nullptr);
		                                    g_render_ctx->TriggerEopEvent();
		                                    return true;
	                                    });

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 8, label_info));

	LabelSet(buffer, label);
}

void GraphicsRenderWriteAtEndOfPipeWithInterruptWriteBackFlip(CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value, int handle,
                                                              int index, int flip_mode, int64_t flip_arg)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	uint64_t args[] = {static_cast<uint64_t>(handle), static_cast<uint64_t>(index), static_cast<uint64_t>(flip_mode),
	                   static_cast<uint64_t>(flip_arg)};

	Graphics::LabelGpuObject label_info(
	    value,
	    [](const uint64_t* args)
	    {
		    EXIT_IF(g_render_ctx == nullptr);
		    GpuMemoryWriteBack(g_render_ctx->GetGraphicCtx());

		    int     handle    = static_cast<int>(args[0]);
		    int     index     = static_cast<int>(args[1]);
		    int     flip_mode = static_cast<int>(args[2]);
		    int64_t flip_arg  = static_cast<int>(args[3]);

		    VideoOut::VideoOutSubmitFlip(handle, index, flip_mode, flip_arg);
		    return true;
	    },
	    [](const uint64_t* /*args*/)
	    {
		    EXIT_IF(g_render_ctx == nullptr);
		    g_render_ctx->TriggerEopEvent();
		    return true;
	    },
	    args);

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 4, label_info));

	LabelSet(buffer, label);
}

void GraphicsRenderWriteAtEndOfPipeWithFlip(CommandBuffer* buffer, uint32_t* dst_gpu_addr, uint32_t value, int handle, int index,
                                            int flip_mode, int64_t flip_arg)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(dst_gpu_addr == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->IsInvalid());

	Core::LockGuard lock(g_render_ctx->GetMutex());

	uint64_t args[] = {static_cast<uint64_t>(handle), static_cast<uint64_t>(index), static_cast<uint64_t>(flip_mode),
	                   static_cast<uint64_t>(flip_arg)};

	Graphics::LabelGpuObject label_info(
	    value,
	    [](const uint64_t* args)
	    {
		    int     handle    = static_cast<int>(args[0]);
		    int     index     = static_cast<int>(args[1]);
		    int     flip_mode = static_cast<int>(args[2]);
		    int64_t flip_arg  = static_cast<int>(args[3]);

		    VideoOut::VideoOutSubmitFlip(handle, index, flip_mode, flip_arg);
		    return true;
	    },
	    nullptr, args);

	auto* label = static_cast<Label*>(
	    GpuMemoryCreateObject(g_render_ctx->GetGraphicCtx(), nullptr, reinterpret_cast<uint64_t>(dst_gpu_addr), 4, label_info));

	LabelSet(buffer, label);
}

void GraphicsRenderWriteBack()
{
	Core::LockGuard lock(g_render_ctx->GetMutex());

	EXIT_IF(g_render_ctx == nullptr);

	GpuMemoryWriteBack(g_render_ctx->GetGraphicCtx());
}

static void eop_event_reset_func(LibKernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

static void eop_event_delete_func(LibKernel::EventQueue::KernelEqueue eq, LibKernel::EventQueue::KernelEqueueEvent* event)
{
	EXIT_IF(event == nullptr);
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_NOT_IMPLEMENTED(event->event.ident != GRAPHICS_EVENT_EOP);
	EXIT_NOT_IMPLEMENTED(event->event.filter != LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS);
	g_render_ctx->DeleteEopEq(eq);
}

static void eop_event_trigger_func(LibKernel::EventQueue::KernelEqueueEvent* event, void* trigger_data)
{
	EXIT_IF(event == nullptr);
	event->triggered = true;
	event->event.fflags++;
	event->event.data = reinterpret_cast<intptr_t>(trigger_data);
}

int GraphicsRenderAddEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id, void* udata)
{
	EXIT_IF(g_render_ctx == nullptr);

	EXIT_NOT_IMPLEMENTED(id != GRAPHICS_EVENT_EOP);

	LibKernel::EventQueue::KernelEqueueEvent event;
	event.triggered                = false;
	event.event.ident              = GRAPHICS_EVENT_EOP;
	event.event.filter             = LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS;
	event.event.udata              = udata;
	event.event.fflags             = 0;
	event.event.data               = 0;
	event.filter.delete_event_func = eop_event_delete_func;
	event.filter.reset_func        = eop_event_reset_func;
	event.filter.trigger_func      = eop_event_trigger_func;
	event.filter.data              = nullptr;

	int result = LibKernel::EventQueue::KernelAddEvent(eq, event);

	g_render_ctx->AddEopEq(eq);

	return result;
}

int GraphicsRenderDeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id)
{
	EXIT_IF(g_render_ctx == nullptr);

	EXIT_NOT_IMPLEMENTED(id != GRAPHICS_EVENT_EOP);

	int result = LibKernel::EventQueue::KernelDeleteEvent(eq, GRAPHICS_EVENT_EOP, LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS);

	g_render_ctx->DeleteEopEq(eq);

	return result;
}

void GraphicsRenderClearGds(uint64_t dw_offset, uint32_t dw_num, uint32_t clear_value)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(g_render_ctx->GetGdsBuffer() == nullptr);

	g_render_ctx->GetGdsBuffer()->Clear(g_render_ctx->GetGraphicCtx(), dw_offset, dw_num, clear_value);
}

void GraphicsRenderReadGds(uint32_t* dst, uint32_t dw_offset, uint32_t dw_size)
{
	EXIT_IF(g_render_ctx == nullptr);
	EXIT_IF(g_render_ctx->GetGdsBuffer() == nullptr);

	g_render_ctx->GetGdsBuffer()->Read(g_render_ctx->GetGraphicCtx(), dst, dw_offset, dw_size);
}

void GraphicsRenderMemoryFree(uint64_t vaddr, uint64_t size)
{
	GpuMemoryFree(g_render_ctx->GetGraphicCtx(), vaddr, size, false);
}

bool CommandBuffer::IsInvalid() const
{
	EXIT_IF(g_render_ctx == nullptr);

	if (m_pool != nullptr)
	{
		Core::LockGuard lock(m_pool->mutex);

		return (m_index == static_cast<uint32_t>(-1) || m_index >= m_pool->buffers_count);
	}

	return true;
}

void CommandBuffer::Allocate()
{
	EXIT_IF(!IsInvalid());

	m_pool = g_command_pool.GetPool();

	Core::LockGuard lock(m_pool->mutex);

	for (uint32_t i = 0; i < m_pool->buffers_count; i++)
	{
		if (!m_pool->busy[i])
		{
			m_pool->busy[i] = true;
			vkResetCommandBuffer(m_pool->buffers[i], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
			m_index = i;
			break;
		}
	}

	EXIT_NOT_IMPLEMENTED(IsInvalid());
}

void CommandBuffer::Free()
{
	EXIT_IF(IsInvalid());

	Core::LockGuard lock(m_pool->mutex);

	WaitForFence();

	m_pool->busy[m_index] = false;
	vkResetCommandBuffer(m_pool->buffers[m_index], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
	m_index = static_cast<uint32_t>(-1);

	EXIT_NOT_IMPLEMENTED(!IsInvalid());
}

void CommandBuffer::Begin() const
{
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];

	VkCommandBufferBeginInfo begin_info {};
	begin_info.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.pNext            = nullptr;
	begin_info.flags            = 0;
	begin_info.pInheritanceInfo = nullptr;

	auto result = vkBeginCommandBuffer(buffer, &begin_info);

	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
}

void CommandBuffer::End() const
{
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];

	auto result = vkEndCommandBuffer(buffer);

	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
}

void CommandBuffer::Execute()
{
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];
	auto* fence  = m_pool->fences[m_index];

	VkSubmitInfo submit_info {};
	submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext                = nullptr;
	submit_info.waitSemaphoreCount   = 0;
	submit_info.pWaitSemaphores      = nullptr;
	submit_info.pWaitDstStageMask    = nullptr;
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &buffer;
	submit_info.signalSemaphoreCount = 0;
	submit_info.pSignalSemaphores    = nullptr;

	EXIT_IF(m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);

	auto* queue = g_render_ctx->GetGraphicCtx()->queue[m_queue];

	auto result = vkQueueSubmit(queue, 1, &submit_info, fence);

	m_execute = true;

	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
}

void CommandBuffer::ExecuteWithSemaphore()
{
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];
	auto* fence  = m_pool->fences[m_index];
	// auto* semaphore = m_pool->semaphores[m_index];

	VkSubmitInfo submit_info {};
	submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.pNext                = nullptr;
	submit_info.waitSemaphoreCount   = 0;
	submit_info.pWaitSemaphores      = nullptr;
	submit_info.pWaitDstStageMask    = nullptr;
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &buffer;
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores    = &m_pool->semaphores[m_index];

	EXIT_IF(m_queue < 0 || m_queue >= GraphicContext::QUEUES_NUM);

	auto* queue = g_render_ctx->GetGraphicCtx()->queue[m_queue];

	auto result = vkQueueSubmit(queue, 1, &submit_info, fence);

	m_execute = true;

	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
}

void CommandBuffer::WaitForFence()
{
	EXIT_IF(IsInvalid());

	if (m_execute)
	{
		auto* device = g_render_ctx->GetGraphicCtx()->device;

		vkWaitForFences(device, 1, &m_pool->fences[m_index], VK_TRUE, UINT64_MAX);
		vkResetFences(device, 1, &m_pool->fences[m_index]);

		m_execute = false;
	}
}

void CommandBuffer::WaitForFenceAndReset()
{
	EXIT_IF(IsInvalid());

	if (m_execute)
	{
		auto* device = g_render_ctx->GetGraphicCtx()->device;

		vkWaitForFences(device, 1, &m_pool->fences[m_index], VK_TRUE, UINT64_MAX);
		vkResetFences(device, 1, &m_pool->fences[m_index]);
		vkResetCommandBuffer(m_pool->buffers[m_index], VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);

		m_execute = false;
	}
}

void CommandBuffer::BeginRenderPass(VulkanFramebuffer* framebuffer, RenderColorInfo* color, RenderDepthInfo* depth) const
{
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];

	EXIT_IF(framebuffer == nullptr);

	bool with_depth = (depth->format != VK_FORMAT_UNDEFINED && depth->vulkan_buffer != nullptr);
	bool with_color = (color->vulkan_buffer != nullptr);

	EXIT_NOT_IMPLEMENTED(!with_depth && !with_color);

	VkClearValue clears[2];
	clears[0].color        = {{0.0f, 0.0f, 0.0f, 1.0f}};
	clears[1].depthStencil = {depth->depth_clear_value, depth->stencil_clear_value};

	VkExtent2D extent = (with_color ? color->vulkan_buffer->extent : depth->vulkan_buffer->extent);

	VkRenderPassBeginInfo render_pass_info {};
	render_pass_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	render_pass_info.pNext             = nullptr;
	render_pass_info.renderPass        = framebuffer->render_pass;
	render_pass_info.framebuffer       = framebuffer->framebuffer;
	render_pass_info.renderArea.offset = {0, 0};
	render_pass_info.renderArea.extent = extent;
	render_pass_info.clearValueCount   = with_depth ? 2 : 1;
	render_pass_info.pClearValues      = clears;

	if (with_color && color->vulkan_buffer->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
	{
		VkImageMemoryBarrier image_memory_barrier {};
		image_memory_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_memory_barrier.pNext                           = nullptr;
		image_memory_barrier.srcAccessMask                   = VK_ACCESS_MEMORY_READ_BIT;
		image_memory_barrier.dstAccessMask                   = VK_ACCESS_MEMORY_WRITE_BIT;
		image_memory_barrier.oldLayout                       = color->vulkan_buffer->layout;
		image_memory_barrier.newLayout                       = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
		image_memory_barrier.image                           = color->vulkan_buffer->image;
		image_memory_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		image_memory_barrier.subresourceRange.baseMipLevel   = 0;
		image_memory_barrier.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;
		image_memory_barrier.subresourceRange.baseArrayLayer = 0;
		image_memory_barrier.subresourceRange.layerCount     = 1;

		vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                     VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		                     &image_memory_barrier);

		color->vulkan_buffer->layout = image_memory_barrier.newLayout;
	}

	vkCmdBeginRenderPass(buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
}

void CommandBuffer::EndRenderPass() const
{
	EXIT_IF(IsInvalid());

	auto* buffer = m_pool->buffers[m_index];

	vkCmdEndRenderPass(buffer);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
