﻿// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for license information.

#include <pch.h>

#include "catalog.h"
#include "TypeResolution.h"

#include <activation.h>
#include <shlwapi.h>
#include <rometadata.h>

#include <wrl.h>

#include <../DynamicDependency/API/MddWinRT.h>

using namespace std;
using namespace Microsoft::WRL;

//TODO Components won't respect COM lifetime. workaround to get them in the COM list? See dev\UndockedRegFreeWinRT\catalog.cpp

extern "C"
{
    typedef HRESULT(__stdcall* activation_factory_type)(HSTRING, IActivationFactory**);
}

// Intentionally no class factory cache here. That would be excessive since
// other layers already cache.
struct component
{
    wstring module_name;
    wstring xmlns;
    HMODULE handle = nullptr;
    activation_factory_type get_activation_factory;
    ABI::Windows::Foundation::ThreadingType threading_model;

    ~component()
    {
        if (handle)
        {
            FreeLibrary(handle);
        }
    }

    HRESULT LoadModule()
    {
        if (handle == nullptr)
        {
            handle = LoadLibraryExW(module_name.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (handle == nullptr)
            {
                return HRESULT_FROM_WIN32(GetLastError());
            }
            this->get_activation_factory = (activation_factory_type)GetProcAddress(handle, "DllGetActivationFactory");
            if (this->get_activation_factory == nullptr)
            {
                return HRESULT_FROM_WIN32(GetLastError());
            }
        }
        return (handle != nullptr && this->get_activation_factory != nullptr) ? S_OK : E_FAIL;
    }

    HRESULT GetActivationFactory(HSTRING className, REFIID  iid, void** factory)
    {
        RETURN_IF_FAILED(LoadModule());

        IActivationFactory* ifactory = nullptr;
        HRESULT hr = this->get_activation_factory(className, &ifactory);
        // optimize for IActivationFactory?
        if (SUCCEEDED(hr))
        {
            hr = ifactory->QueryInterface(iid, factory);
            ifactory->Release();
        }
        return hr;
    }
};

static unordered_map<wstring, shared_ptr<component>> g_types;

HRESULT LoadManifestFromPath(std::wstring path)
{
    if (path.size() < 4)
    {
        return COR_E_ARGUMENT;
    }
    std::wstring ext(path.substr(path.size() - 4, path.size()));
    if ((CompareStringOrdinal(ext.c_str(), -1, L".exe", -1, TRUE) == CSTR_EQUAL) ||
        (CompareStringOrdinal(ext.c_str(), -1, L".dll", -1, TRUE) == CSTR_EQUAL))
    {
        return LoadFromEmbeddedManifest(path.c_str());
    }
    else
    {
        return LoadFromSxSManifest(path.c_str());
    }
}

HRESULT LoadFromSxSManifest(PCWSTR path)
{
    return WinRTLoadComponentFromFilePath(path);
}

HRESULT LoadFromEmbeddedManifest(PCWSTR path)
{
    wil::unique_hmodule handle(LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE));
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !handle);

    // Try both just to be on the safe side
    HRSRC hrsc = FindResourceW(handle.get(), MAKEINTRESOURCEW(1), RT_MANIFEST);
    if (!hrsc)
    {
        hrsc = FindResourceW(handle.get(), MAKEINTRESOURCEW(2), RT_MANIFEST);
        RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !hrsc);
    }
    HGLOBAL embeddedManifest = LoadResource(handle.get(), hrsc);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !embeddedManifest);

    DWORD length = SizeofResource(handle.get(), hrsc);
    void* data = LockResource(embeddedManifest);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !data);

    return WinRTLoadComponentFromString(std::string_view((char*)data, length));
}

HRESULT WinRTLoadComponentFromFilePath(PCWSTR manifestPath)
{
    ComPtr<IStream> fileStream;
    RETURN_IF_FAILED(SHCreateStreamOnFileEx(manifestPath, STGM_READ, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &fileStream));
    try
    {
        return ParseRootManifestFromXmlReaderInput(fileStream.Get());
    }
    catch(...)
    {
        LOG_CAUGHT_EXCEPTION();
        return ERROR_SXS_MANIFEST_PARSE_ERROR;
    }
}

HRESULT WinRTLoadComponentFromString(std::string_view xmlStringValue)
{
    ComPtr<IStream> xmlStream;
    auto xmlStringValueData{ xmlStringValue.data() };
    auto xmlStringValueDataLength{ strlen(xmlStringValueData) };
    auto xmlStringValueDataSize{ xmlStringValueDataLength * sizeof(*xmlStringValueData) };
    xmlStream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(xmlStringValue.data()), static_cast<UINT>(xmlStringValueDataSize)));
    RETURN_HR_IF_NULL(E_OUTOFMEMORY, xmlStream);
    ComPtr<IXmlReaderInput> xmlReaderInput;
    RETURN_IF_FAILED(CreateXmlReaderInputWithEncodingName(xmlStream.Get(), nullptr, L"utf-8", FALSE, nullptr, &xmlReaderInput));
    try
    {
        return ParseRootManifestFromXmlReaderInput(xmlReaderInput.Get());
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        return ERROR_SXS_MANIFEST_PARSE_ERROR;
    }
}

HRESULT ParseRootManifestFromXmlReaderInput(IUnknown* input)
{
    XmlNodeType nodeType;
    PCWSTR localName = nullptr;
    auto locale = _create_locale(LC_ALL, "C");
    ComPtr<IXmlReader> xmlReader;
    RETURN_IF_FAILED(CreateXmlReader(__uuidof(IXmlReader), (void**)&xmlReader, nullptr));
    RETURN_IF_FAILED(xmlReader->SetInput(input));
    while (S_OK == xmlReader->Read(&nodeType))
    {
        if (nodeType == XmlNodeType_Element)
        {
            RETURN_IF_FAILED((xmlReader->GetLocalName(&localName, nullptr)));

            if (_wcsicmp_l(localName, L"file", locale) == 0)
            {
                RETURN_IF_FAILED(ParseFileTag(xmlReader.Get()));
            }
        }
    }

    return S_OK;
}

HRESULT ParseFileTag(IXmlReader* xmlReader)
{
    HRESULT hr = S_OK;
    XmlNodeType nodeType;
    PCWSTR localName = nullptr;
    PCWSTR value = nullptr;
    hr = xmlReader->MoveToAttributeByName(L"name", nullptr);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_SXS_MANIFEST_PARSE_ERROR), hr != S_OK);
    RETURN_IF_FAILED(xmlReader->GetValue(&value, nullptr));
    std::wstring fileName{ !value ? L"" : value };
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_SXS_MANIFEST_PARSE_ERROR), fileName.empty());
    auto locale = _create_locale(LC_ALL, "C");
    while (S_OK == xmlReader->Read(&nodeType))
    {
        if (nodeType == XmlNodeType_Element)
        {
            RETURN_IF_FAILED(xmlReader->GetLocalName(&localName, nullptr));
            if (localName != nullptr && _wcsicmp_l(localName, L"activatableClass", locale) == 0)
            {
                RETURN_IF_FAILED(ParseActivatableClassTag(xmlReader, fileName.c_str()));
            }
        }
        else if (nodeType == XmlNodeType_EndElement)
        {
            RETURN_IF_FAILED(xmlReader->GetLocalName(&localName, nullptr));
            RETURN_HR_IF(S_OK, localName != nullptr && _wcsicmp_l(localName, L"file", locale) == 0);
        }
    }
    return S_OK;
}

HRESULT ParseActivatableClassTag(IXmlReader* xmlReader, PCWSTR fileName)
{
    auto locale = _create_locale(LC_ALL, "C");
    auto this_component = make_shared<component>();
    this_component->module_name = fileName;
    HRESULT hr = xmlReader->MoveToFirstAttribute();
    // Using this pattern intead of calling multiple MoveToAttributeByName improves performance
    const WCHAR* activatableClass = nullptr;
    const WCHAR* threadingModel = nullptr;
    const WCHAR* xmlns = nullptr;
    if (S_FALSE == hr)
    {
        return HRESULT_FROM_WIN32(ERROR_SXS_MANIFEST_PARSE_ERROR);
    }
    else
    {
        while (TRUE)
        {
            const WCHAR* pwszLocalName;
            const WCHAR* pwszValue;
            if (FAILED_LOG(xmlReader->GetLocalName(&pwszLocalName, NULL)))
            {
                return HRESULT_FROM_WIN32(ERROR_SXS_MANIFEST_PARSE_ERROR);
            }
            if (FAILED_LOG(xmlReader->GetValue(&pwszValue, NULL)))
            {
                return HRESULT_FROM_WIN32(ERROR_SXS_MANIFEST_PARSE_ERROR);
            }
            if (pwszLocalName != nullptr)
            {
                if (_wcsicmp_l(L"threadingModel", pwszLocalName, locale) == 0)
                {
                    threadingModel = pwszValue;
                }
                else if (_wcsicmp_l(L"name", pwszLocalName, locale) == 0)
                {
                    activatableClass = pwszValue;
                }
                else if (_wcsicmp_l(L"xmlns", pwszLocalName, locale) == 0)
                {
                    xmlns = pwszValue;
                }
            }
            if (xmlReader->MoveToNextAttribute() != S_OK)
            {
                break;
            }
        }
    }
    if (threadingModel == nullptr)
    {
        return HRESULT_FROM_WIN32(ERROR_SXS_MANIFEST_PARSE_ERROR);
    }
    if (_wcsicmp_l(L"sta", threadingModel, locale) == 0)
    {
        this_component->threading_model = ABI::Windows::Foundation::ThreadingType::ThreadingType_STA;
    }
    else if (_wcsicmp_l(L"mta", threadingModel, locale) == 0)
    {
        this_component->threading_model = ABI::Windows::Foundation::ThreadingType::ThreadingType_MTA;
    }
    else if (_wcsicmp_l(L"both", threadingModel, locale) == 0)
    {
        this_component->threading_model = ABI::Windows::Foundation::ThreadingType::ThreadingType_BOTH;
    }
    else
    {
        return HRESULT_FROM_WIN32(ERROR_SXS_MANIFEST_PARSE_ERROR);
    }

    if (activatableClass == nullptr || !activatableClass[0])
    {
        return HRESULT_FROM_WIN32(ERROR_SXS_MANIFEST_PARSE_ERROR);
    }
    this_component->xmlns = xmlns; // Should we care if this value is blank or missing?
    // Check for duplicate activatable classes
    auto component_iter = g_types.find(activatableClass);
    if (component_iter != g_types.end())
    {
        return HRESULT_FROM_WIN32(ERROR_SXS_DUPLICATE_ACTIVATABLE_CLASS);
    }
    g_types[activatableClass] = this_component;
    return S_OK;
}

HRESULT WinRTGetThreadingModel(HSTRING activatableClassId, ABI::Windows::Foundation::ThreadingType* threading_model)
{
    HRESULT hr{ WinRTGetThreadingModel_PackageGraph(activatableClassId, threading_model) };
    if (hr == REGDB_E_CLASSNOTREG)  // Not found
    {
        hr = WinRTGetThreadingModel_SxS(activatableClassId, threading_model);
    }
    return hr;
}

HRESULT WinRTGetThreadingModel_PackageGraph(HSTRING activatableClassId, ABI::Windows::Foundation::ThreadingType* threading_model)
{
    return MddCore::WinRT::GetThreadingModel(activatableClassId, *threading_model);
}

HRESULT WinRTGetThreadingModel_SxS(HSTRING activatableClassId, ABI::Windows::Foundation::ThreadingType* threading_model)
{
    auto raw_class_name = WindowsGetStringRawBuffer(activatableClassId, nullptr);
    auto component_iter = g_types.find(raw_class_name);
    if (component_iter != g_types.end())
    {
        *threading_model = component_iter->second->threading_model;
        return S_OK;
    }
    return REGDB_E_CLASSNOTREG;
}

HRESULT WinRTGetActivationFactory(
    HSTRING activatableClassId,
    REFIID iid,
    void** factory)
{
    RETURN_IF_FAILED(WinRTGetActivationFactory_PackageGraph(activatableClassId, iid, factory));
    if (*factory == nullptr)    // Not found
    {
        RETURN_IF_FAILED(WinRTGetActivationFactory_SxS(activatableClassId, iid, factory));
    }
    return S_OK;

}

HRESULT WinRTGetActivationFactory_PackageGraph(
    HSTRING activatableClassId,
    REFIID iid,
    void** factory)
{
    RETURN_IF_FAILED(MddCore::WinRT::GetActivationFactory(activatableClassId, iid, factory));
    return S_OK;
}

HRESULT WinRTGetActivationFactory_SxS(
    HSTRING activatableClassId,
    REFIID iid,
    void** factory)
{
    auto raw_class_name = WindowsGetStringRawBuffer(activatableClassId, nullptr);
    auto component_iter = g_types.find(raw_class_name);
    if (component_iter != g_types.end())
    {
        return component_iter->second->GetActivationFactory(activatableClassId, iid, factory);
    }
    return REGDB_E_CLASSNOTREG;
}

HRESULT WinRTGetMetadataFile(
    const HSTRING name,
    IMetaDataDispenserEx* metaDataDispenser,
    HSTRING* metaDataFilePath,
    IMetaDataImport2** metaDataImport,
    mdTypeDef* typeDefToken)
{
    wchar_t folderPrefix[9];
    PCWSTR pszFullName = WindowsGetStringRawBuffer(name, nullptr);
    HRESULT hr = StringCchCopyW(folderPrefix, _countof(folderPrefix), pszFullName);
    if (hr != S_OK && hr != STRSAFE_E_INSUFFICIENT_BUFFER)
    {
        return hr;
    }
    if (CompareStringOrdinal(folderPrefix, -1, L"Windows.", -1, FALSE) == CSTR_EQUAL)
    {
        return RO_E_METADATA_NAME_NOT_FOUND;
    }

    if (metaDataFilePath != nullptr)
    {
        *metaDataFilePath = nullptr;
    }
    if (metaDataImport != nullptr)
    {
        *metaDataImport = nullptr;
    }
    if (typeDefToken != nullptr)
    {
        *typeDefToken = mdTypeDefNil;
    }

    if (((metaDataImport == nullptr) && (typeDefToken != nullptr)) ||
        ((metaDataImport != nullptr) && (typeDefToken == nullptr)))
    {
        return E_INVALIDARG;
    }

    ComPtr<IMetaDataDispenserEx> spMetaDataDispenser;
    // The API uses the caller's passed-in metadata dispenser. If null, it
    // will create an instance of the metadata reader to dispense metadata files.
    if (metaDataDispenser == nullptr)
    {
        // Avoid using CoCreateInstance here, which will trigger a Feature-On-Demand (FOD)
        // installation request of .NET Framework 3.5 if it's not already installed. In the
        // mean time, Windows App SDK runtime doesn't need .NET Framework 3.5.
        RETURN_IF_FAILED(MetaDataGetDispenser(CLSID_CorMetaDataDispenser,
            IID_IMetaDataDispenser,
            &spMetaDataDispenser));
        {
            auto versionString{ wil::make_bstr_nothrow(L"WindowsRuntime 1.4") };
            RETURN_IF_NULL_ALLOC(versionString);
            VARIANT version{};
            version.vt = VT_BSTR;
            version.bstrVal = versionString.get();
            RETURN_IF_FAILED(spMetaDataDispenser->SetOption(MetaDataRuntimeVersion, &version));
        }
    }
    return UndockedRegFreeWinRT::ResolveThirdPartyType(
        spMetaDataDispenser.Get(),
        pszFullName,
        metaDataFilePath,
        metaDataImport,
        typeDefToken);
}
