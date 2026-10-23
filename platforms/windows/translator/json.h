// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <stdexcept>
namespace transinput::translation {
namespace json=winrt::Windows::Data::Json;
inline constexpr wchar_t Model[]=L"deepseek-flash";
inline constexpr size_t MaxBody=32768,MaxOutput=4096;
inline std::string RequestBody(std::string_view prompt,std::string_view source) {
    json::JsonObject request;json::JsonArray messages;
    for(const auto& [role,content]:{std::pair{L"system",prompt},std::pair{L"user",source}}){
        json::JsonObject message;
        message.Insert(L"role",json::JsonValue::CreateStringValue(role));
        message.Insert(L"content",json::JsonValue::CreateStringValue(winrt::to_hstring(content)));
        messages.Append(message);
    }
    request.Insert(L"model",json::JsonValue::CreateStringValue(Model));
    request.Insert(L"messages",messages);
    json::JsonObject thinking;thinking.Insert(L"type",json::JsonValue::CreateStringValue(L"disabled"));
    request.Insert(L"thinking",thinking);
    request.Insert(L"stream",json::JsonValue::CreateBooleanValue(false));
    request.Insert(L"temperature",json::JsonValue::CreateNumberValue(0.2));
    request.Insert(L"max_tokens",json::JsonValue::CreateNumberValue(1024));
    return winrt::to_string(request.Stringify());
}
inline std::string ParseReply(std::string_view body) {
    if(body.empty()||body.size()>MaxBody)throw std::runtime_error("translation response size");
    json::JsonObject object;
    if(!json::JsonObject::TryParse(winrt::to_hstring(body),object)||object.HasKey(L"error"))throw std::runtime_error("translation response JSON");
    const auto choices=object.GetNamedArray(L"choices");
    if(choices.Size()!=1)throw std::runtime_error("translation response choices");
    const auto choice=choices.GetObjectAt(0);
    if(choice.GetNamedNumber(L"index")!=0||choice.GetNamedString(L"finish_reason")!=L"stop")throw std::runtime_error("translation unfinished");
    const auto message=choice.GetNamedObject(L"message");
    if(message.GetNamedString(L"role")!=L"assistant"||message.HasKey(L"function_call"))throw std::runtime_error("translation response role");
    if(message.HasKey(L"tool_calls")&&message.GetNamedArray(L"tool_calls").Size()!=0)throw std::runtime_error("translation tool response");
    if(message.HasKey(L"reasoning_content")){
        const auto reasoning=message.Lookup(L"reasoning_content");
        if(reasoning.ValueType()!=json::JsonValueType::Null&&reasoning.GetString()!=L"")throw std::runtime_error("translation thinking response");
    }
    auto text=winrt::to_string(message.GetNamedString(L"content"));
    if(text.empty()||text.size()>MaxOutput||text.find_first_not_of(" \r\n\t")==std::string::npos||
        std::any_of(text.begin(),text.end(),[](unsigned char c){return(c<32&&c!='\n'&&c!='\t')||c==127;}))throw std::runtime_error("translation text size or control");
    return text;
}
}
