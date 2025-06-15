#ifndef BLUR_HPP
#define BLUR_HPP

#include <d3d11.h>
#include <d3dcompiler.h>
#include <imgui.h>
#include <algorithm>

#undef min
#undef max

namespace blur {

    struct blur_params {
        ID3D11Device* device;
        ImDrawList* draw_list;
        ImVec2 window_pos;
        ImVec2 window_size;
        float blur_strength = 0.95f;
        float corner_radius = 6.0f;
        double delay_time = 0.15;
    };

    struct blur_constants {
        float texture_size[2];
        float blur_strength;
        float padding;
    };

    struct vertex {
        float position[3];
        float uv[2];
    };

    class blur_renderer {
    private:
        ID3D11Device* device_ = nullptr;
        ID3D11DeviceContext* context_ = nullptr;

        ID3D11VertexShader* vertex_shader_ = nullptr;
        ID3D11PixelShader* pixel_shader_horizontal_ = nullptr;
        ID3D11PixelShader* pixel_shader_vertical_ = nullptr;
        ID3D11Buffer* vertex_buffer_ = nullptr;
        ID3D11Buffer* constant_buffer_ = nullptr;
        ID3D11InputLayout* input_layout_ = nullptr;
        ID3D11SamplerState* sampler_state_ = nullptr;
        ID3D11BlendState* blend_state_ = nullptr;
        ID3D11RasterizerState* rasterizer_state_ = nullptr;

        ID3D11Texture2D* background_capture_ = nullptr;
        ID3D11ShaderResourceView* background_srv_ = nullptr;
        ID3D11Texture2D* temp_texture_ = nullptr;
        ID3D11RenderTargetView* temp_rtv_ = nullptr;
        ID3D11ShaderResourceView* temp_srv_ = nullptr;
        ID3D11Texture2D* blur_texture_ = nullptr;
        ID3D11RenderTargetView* blur_rtv_ = nullptr;
        ID3D11ShaderResourceView* blur_srv_ = nullptr;

        int width_ = 0;
        int height_ = 0;
        bool initialized_ = false;
        bool background_captured_ = false;
        bool blur_processed_ = false;
        bool blur_enabled_last_frame_ = false;
        double blur_enable_time_ = 0.0;
        bool blur_capture_pending_ = false;

        bool initialize_shaders();
        bool initialize_render_states();
        bool ensure_render_targets(int width, int height);
        bool capture_background(ImVec2 window_pos, ImVec2 window_size);
        bool process_blur(float blur_strength);
        void reset_state();
        void cleanup_render_targets();
        void cleanup_all();

        static constexpr const char* vertex_shader_source_ = R"(
        struct VS_INPUT { float3 position : POSITION; float2 uv : TEXCOORD0; };
        struct VS_OUTPUT { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
        VS_OUTPUT main(VS_INPUT input) {
            VS_OUTPUT output;
            output.position = float4(input.position, 1.0f);
            output.uv = input.uv;
            return output;
        })";

        static constexpr const char* horizontal_blur_source_ = R"(
        cbuffer BlurConstants : register(b0) { float2 texture_size; float blur_strength; float padding; };
        Texture2D source_texture : register(t0);
        SamplerState texture_sampler : register(s0);
        struct PS_INPUT { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
        float4 main(PS_INPUT input) : SV_Target {
            float4 color = float4(0.0f, 0.0f, 0.0f, 0.0f);
            float pixel_size = 1.0f / texture_size.x;
            float total_weight = 0.0f;
            int radius = 4;
            for (int i = -radius; i <= radius; i++) {
                float2 sample_uv = input.uv + float2(pixel_size * i * blur_strength, 0.0f);
                sample_uv = clamp(sample_uv, float2(0.0f, 0.0f), float2(1.0f, 1.0f));
                float weight = exp(-0.5f * (i * i) / (radius * radius * 0.5f));
                color += source_texture.Sample(texture_sampler, sample_uv) * weight;
                total_weight += weight;
            }
            return color / total_weight;
        })";

        static constexpr const char* vertical_blur_source_ = R"(
        cbuffer BlurConstants : register(b0) { float2 texture_size; float blur_strength; float padding; };
        Texture2D source_texture : register(t0);
        SamplerState texture_sampler : register(s0);
        struct PS_INPUT { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
        float4 main(PS_INPUT input) : SV_Target {
            float4 color = float4(0.0f, 0.0f, 0.0f, 0.0f);
            float pixel_size = 1.0f / texture_size.y;
            float total_weight = 0.0f;
            int radius = 4;
            for (int i = -radius; i <= radius; i++) {
                float2 sample_uv = input.uv + float2(0.0f, pixel_size * i * blur_strength);
                sample_uv = clamp(sample_uv, float2(0.0f, 0.0f), float2(1.0f, 1.0f));
                float weight = exp(-0.5f * (i * i) / (radius * radius * 0.5f));
                color += source_texture.Sample(texture_sampler, sample_uv) * weight;
                total_weight += weight;
            }
            return color / total_weight;
        })";

    public:
        ~blur_renderer() { cleanup_all(); }
        bool render(const blur_params& params, bool should_blur);
    };

    bool blur_renderer::render(const blur_params& params, bool should_blur) {
        if (!params.device || !params.draw_list) return false;

        if (!initialized_ || device_ != params.device) {
            cleanup_all();
            device_ = params.device;
            device_->GetImmediateContext(&context_);

            if (!initialize_shaders() || !initialize_render_states()) {
                return false;
            }
            initialized_ = true;
        }

        int window_width = static_cast<int>(params.window_size.x);
        int window_height = static_cast<int>(params.window_size.y);

        if (window_width <= 0 || window_height <= 0) return false;

        double current_time = ImGui::GetTime();

        if (width_ != window_width || height_ != window_height) {
            cleanup_render_targets();
            reset_state();
            width_ = window_width;
            height_ = window_height;
        }

        if (should_blur && !blur_enabled_last_frame_) {
            reset_state();
            blur_capture_pending_ = true;
            blur_enable_time_ = current_time;
        }
        else if (!should_blur && blur_enabled_last_frame_) {
            reset_state();
        }

        if (should_blur && blur_capture_pending_ && !blur_processed_) {
            if (current_time - blur_enable_time_ >= params.delay_time) {
                blur_capture_pending_ = false;

                if (ensure_render_targets(window_width, window_height)) {
                    if (capture_background(params.window_pos, params.window_size)) {
                        process_blur(params.blur_strength);
                    }
                }
            }
        }

        blur_enabled_last_frame_ = should_blur;

        if (should_blur && blur_processed_ && blur_srv_) {
            params.draw_list->AddImageRounded(
                (void*)blur_srv_,
                params.window_pos,
                ImVec2(params.window_pos.x + params.window_size.x,
                    params.window_pos.y + params.window_size.y),
                ImVec2(0, 0), ImVec2(1, 1),
                IM_COL32(255, 255, 255, 255),
                params.corner_radius
            );
        }

        return true;
    }

    bool blur_renderer::initialize_shaders() {
        auto compile_shader = [](const char* source, const char* target) -> ID3DBlob* {
            ID3DBlob* blob = nullptr;
            ID3DBlob* error = nullptr;

            HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
                "main", target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &error);

            if (error) { error->Release(); }
            return SUCCEEDED(hr) ? blob : nullptr;
            };

        ID3DBlob* vs_blob = compile_shader(vertex_shader_source_, "vs_5_0");
        ID3DBlob* ps_h_blob = compile_shader(horizontal_blur_source_, "ps_5_0");
        ID3DBlob* ps_v_blob = compile_shader(vertical_blur_source_, "ps_5_0");

        if (!vs_blob || !ps_h_blob || !ps_v_blob) {
            if (vs_blob) vs_blob->Release();
            if (ps_h_blob) ps_h_blob->Release();
            if (ps_v_blob) ps_v_blob->Release();
            return false;
        }

        bool success =
            SUCCEEDED(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vertex_shader_)) &&
            SUCCEEDED(device_->CreatePixelShader(ps_h_blob->GetBufferPointer(), ps_h_blob->GetBufferSize(), nullptr, &pixel_shader_horizontal_)) &&
            SUCCEEDED(device_->CreatePixelShader(ps_v_blob->GetBufferPointer(), ps_v_blob->GetBufferSize(), nullptr, &pixel_shader_vertical_));

        if (success) {
            D3D11_INPUT_ELEMENT_DESC layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
            };
            success = SUCCEEDED(device_->CreateInputLayout(layout, 2, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &input_layout_));
        }

        vs_blob->Release();
        ps_h_blob->Release();
        ps_v_blob->Release();

        return success;
    }

    bool blur_renderer::initialize_render_states() {
        vertex vertices[] = {
            {{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
            {{-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f}},
            {{ 1.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
            {{ 1.0f,  1.0f, 0.0f}, {1.0f, 0.0f}}
        };

        D3D11_BUFFER_DESC buffer_desc = {};
        buffer_desc.Usage = D3D11_USAGE_DEFAULT;
        buffer_desc.ByteWidth = sizeof(vertices);
        buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA init_data = {};
        init_data.pSysMem = vertices;

        if (FAILED(device_->CreateBuffer(&buffer_desc, &init_data, &vertex_buffer_))) {
            return false;
        }

        buffer_desc.ByteWidth = sizeof(blur_constants);
        buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
        buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(device_->CreateBuffer(&buffer_desc, nullptr, &constant_buffer_))) {
            return false;
        }

        D3D11_SAMPLER_DESC sampler_desc = {};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        sampler_desc.MaxAnisotropy = 1;

        if (FAILED(device_->CreateSamplerState(&sampler_desc, &sampler_state_))) {
            return false;
        }

        D3D11_BLEND_DESC blend_desc = {};
        blend_desc.RenderTarget[0].BlendEnable = TRUE;
        blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        if (FAILED(device_->CreateBlendState(&blend_desc, &blend_state_))) {
            return false;
        }

        D3D11_RASTERIZER_DESC raster_desc = {};
        raster_desc.FillMode = D3D11_FILL_SOLID;
        raster_desc.CullMode = D3D11_CULL_NONE;
        raster_desc.DepthClipEnable = TRUE;

        return SUCCEEDED(device_->CreateRasterizerState(&raster_desc, &rasterizer_state_));
    }

    bool blur_renderer::ensure_render_targets(int width, int height) {
        if (background_capture_ && width_ == width && height_ == height) {
            return true;
        }

        cleanup_render_targets();

        auto create_texture = [this](int w, int h, UINT bind_flags, ID3D11Texture2D** texture,
            ID3D11RenderTargetView** rtv = nullptr, ID3D11ShaderResourceView** srv = nullptr) -> bool {
                D3D11_TEXTURE2D_DESC desc = {};
                desc.Width = w;
                desc.Height = h;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = bind_flags;

                if (FAILED(device_->CreateTexture2D(&desc, nullptr, texture))) return false;
                if (rtv && FAILED(device_->CreateRenderTargetView(*texture, nullptr, rtv))) return false;
                if (srv && FAILED(device_->CreateShaderResourceView(*texture, nullptr, srv))) return false;
                return true;
            };

        return create_texture(width, height, D3D11_BIND_SHADER_RESOURCE, &background_capture_, nullptr, &background_srv_) &&
            create_texture(width, height, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &temp_texture_, &temp_rtv_, &temp_srv_) &&
            create_texture(width, height, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &blur_texture_, &blur_rtv_, &blur_srv_);
    }

    bool blur_renderer::capture_background(ImVec2 window_pos, ImVec2 window_size) {
        ID3D11RenderTargetView* current_rtv = nullptr;
        ID3D11DepthStencilView* current_dsv = nullptr;
        context_->OMGetRenderTargets(1, &current_rtv, &current_dsv);

        if (!current_rtv) return false;

        ID3D11Resource* back_buffer = nullptr;
        current_rtv->GetResource(&back_buffer);

        bool success = false;
        if (back_buffer) {
            D3D11_BOX src_box = {};
            src_box.left = static_cast<UINT>(std::max(0.0f, window_pos.x));
            src_box.top = static_cast<UINT>(std::max(0.0f, window_pos.y));
            src_box.right = static_cast<UINT>(window_pos.x + window_size.x);
            src_box.bottom = static_cast<UINT>(window_pos.y + window_size.y);
            src_box.front = 0;
            src_box.back = 1;

            context_->CopySubresourceRegion(background_capture_, 0, 0, 0, 0, back_buffer, 0, &src_box);
            background_captured_ = true;
            success = true;
            back_buffer->Release();
        }

        if (current_rtv) current_rtv->Release();
        if (current_dsv) current_dsv->Release();
        return success;
    }

    bool blur_renderer::process_blur(float blur_strength) {
        if (!background_captured_) return false;

        ID3D11RenderTargetView* original_rtv = nullptr;
        ID3D11DepthStencilView* original_dsv = nullptr;
        context_->OMGetRenderTargets(1, &original_rtv, &original_dsv);

        D3D11_VIEWPORT original_viewport;
        UINT num_viewports = 1;
        context_->RSGetViewports(&num_viewports, &original_viewport);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context_->Map(constant_buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            blur_constants* constants = static_cast<blur_constants*>(mapped.pData);
            constants->texture_size[0] = static_cast<float>(width_);
            constants->texture_size[1] = static_cast<float>(height_);
            constants->blur_strength = blur_strength;
            constants->padding = 0.0f;
            context_->Unmap(constant_buffer_, 0);
        }

        UINT stride = sizeof(vertex);
        UINT offset = 0;
        context_->IASetVertexBuffers(0, 1, &vertex_buffer_, &stride, &offset);
        context_->IASetInputLayout(input_layout_);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->VSSetShader(vertex_shader_, nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, &constant_buffer_);
        context_->PSSetSamplers(0, 1, &sampler_state_);
        context_->RSSetState(rasterizer_state_);
        context_->OMSetBlendState(blend_state_, nullptr, 0xFFFFFFFF);

        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(width_);
        viewport.Height = static_cast<float>(height_);
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);

        context_->OMSetRenderTargets(1, &temp_rtv_, nullptr);
        context_->PSSetShader(pixel_shader_horizontal_, nullptr, 0);
        context_->PSSetShaderResources(0, 1, &background_srv_);
        context_->Draw(4, 0);

        context_->OMSetRenderTargets(1, &blur_rtv_, nullptr);
        context_->PSSetShader(pixel_shader_vertical_, nullptr, 0);
        context_->PSSetShaderResources(0, 1, &temp_srv_);
        context_->Draw(4, 0);

        context_->OMSetRenderTargets(1, &original_rtv, original_dsv);
        context_->RSSetViewports(1, &original_viewport);

        ID3D11ShaderResourceView* null_srvs[1] = { nullptr };
        context_->PSSetShaderResources(0, 1, null_srvs);

        if (original_rtv) original_rtv->Release();
        if (original_dsv) original_dsv->Release();

        blur_processed_ = true;
        return true;
    }

    void blur_renderer::reset_state() {
        background_captured_ = false;
        blur_processed_ = false;
        blur_capture_pending_ = false;
    }

    void blur_renderer::cleanup_render_targets() {
        if (background_capture_) { background_capture_->Release(); background_capture_ = nullptr; }
        if (background_srv_) { background_srv_->Release(); background_srv_ = nullptr; }
        if (temp_texture_) { temp_texture_->Release(); temp_texture_ = nullptr; }
        if (temp_rtv_) { temp_rtv_->Release(); temp_rtv_ = nullptr; }
        if (temp_srv_) { temp_srv_->Release(); temp_srv_ = nullptr; }
        if (blur_texture_) { blur_texture_->Release(); blur_texture_ = nullptr; }
        if (blur_rtv_) { blur_rtv_->Release(); blur_rtv_ = nullptr; }
        if (blur_srv_) { blur_srv_->Release(); blur_srv_ = nullptr; }
    }

    void blur_renderer::cleanup_all() {
        cleanup_render_targets();

        if (vertex_shader_) { vertex_shader_->Release(); vertex_shader_ = nullptr; }
        if (pixel_shader_horizontal_) { pixel_shader_horizontal_->Release(); pixel_shader_horizontal_ = nullptr; }
        if (pixel_shader_vertical_) { pixel_shader_vertical_->Release(); pixel_shader_vertical_ = nullptr; }
        if (vertex_buffer_) { vertex_buffer_->Release(); vertex_buffer_ = nullptr; }
        if (constant_buffer_) { constant_buffer_->Release(); constant_buffer_ = nullptr; }
        if (input_layout_) { input_layout_->Release(); input_layout_ = nullptr; }
        if (sampler_state_) { sampler_state_->Release(); sampler_state_ = nullptr; }
        if (blend_state_) { blend_state_->Release(); blend_state_ = nullptr; }
        if (rasterizer_state_) { rasterizer_state_->Release(); rasterizer_state_ = nullptr; }

        if (context_) { context_->Release(); context_ = nullptr; }

        initialized_ = false;
        device_ = nullptr;
    }

    inline blur_renderer g_blur_renderer;

    inline bool render_blur_overlay(const blur_params& params, bool should_blur) {
        return g_blur_renderer.render(params, should_blur);
    }

}

#endif