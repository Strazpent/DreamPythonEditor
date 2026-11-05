// SPDX-License-Identifier: Apache-2.0
// The live resolver helpers of §12.1.3 as typed wrappers (A15): _impl.resolver.complete, prewarm and unreal_names
// (TDD §7.5.6, D6, RV-23). Game thread, only from FDreamDeferral idle ticks that need Python (INV-6), never from a Slate
// handler (D14(5)) and never while a run is active or a pump is on the stack. Each call builds the request dict, calls the
// helper with the GIL held and converts the result to Completion/DreamCompletionTypes.h structs; no PyObject outlives the
// call (the helper callables are looked up in sys.modules per call, so nothing needs releasing in HandleShutdown).
// Completion group; the declarations are FDreamPython members (Python/DreamPython.h, "Live completion" block).
#include "Python/DreamPython.h"

#include "Completion/DreamCompletionTypes.h"
#include "HAL/PlatformTime.h"
#include "Module/DreamEditorModule.h"
#include "Python/DreamGil.h"
#include "Python/DreamPyUtil.h"
#include "Run/RunSupervisor.h"
#include "Run/DreamRunService.h"

#include <atomic>
#include <cstring>

namespace DreamPythonCompletionPrivate
{
	/** Resolver helper calls in this process (T-REPL-013's probe: none may come from GetSuggestedCompletions). */
	std::atomic<uint64> GResolverCalls{ 0 };

	/** INV-6, D14(5): Python Ready, no run active, no pump on the stack, game thread. */
	bool CanCallResolver(const FDreamPython& Python)
	{
		if (!IsInGameThread() || !Python.IsReady() || !FDreamEditorModule::IsAvailable())
		{
			return false;
		}
		FDreamEditorModule& Module = FDreamEditorModule::Get();
		return !Module.GetRunService().IsRunActive() && Module.GetSupervisor().GetPumpDepth() == 0;
	}

#if WITH_PYTHON
	/** dream_editor._impl.resolver.<Function>, a new reference, or null with a Python error set. */
	PyObject* GetResolverFunction(const char* Function)
	{
		PyObject* Name = PyUnicode_FromString("dream_editor._impl.resolver");
		if (Name == nullptr)
		{
			return nullptr;
		}
		PyObject* Module = PyImport_GetModule(Name);      // sys.modules lookup; no import machinery when loaded
		Py_DECREF(Name);
		if (Module == nullptr)
		{
			if (PyErr_Occurred())
			{
				return nullptr;
			}
			Module = PyImport_ImportModule("dream_editor._impl.resolver");
			if (Module == nullptr)
			{
				return nullptr;
			}
		}
		PyObject* Callable = PyObject_GetAttrString(Module, Function);
		Py_DECREF(Module);
		return Callable;
	}

	PyObject* FromUtf8(const std::string& Text)
	{
		return PyUnicode_DecodeUTF8(Text.data(), static_cast<Py_ssize_t>(Text.size()), "replace");
	}

	/** Dict[Key] = Value, stealing Value; false on failure. */
	bool SetItem(PyObject* Dict, const char* Key, PyObject* Value)
	{
		if (Value == nullptr)
		{
			return false;
		}
		const bool bOk = PyDict_SetItemString(Dict, Key, Value) == 0;
		Py_DECREF(Value);
		return bOk;
	}

	PyObject* StringList(const TArray<FString>& Strings)
	{
		PyObject* List = PyList_New(0);
		for (int32 Index = 0; List != nullptr && Index < Strings.Num(); ++Index)
		{
			PyObject* Item = DreamPy::FromString(Strings[Index]);
			if (Item == nullptr || PyList_Append(List, Item) != 0)
			{
				Py_XDECREF(Item);
				Py_DECREF(List);
				return nullptr;
			}
			Py_DECREF(Item);
		}
		return List;
	}

	/** [[kind, value], ...] with the final ["prefix", text] (resolver.complete's "chain"). */
	PyObject* BuildChain(const dream::CompletionChain& Chain)
	{
		PyObject* List = PyList_New(0);
		if (List == nullptr)
		{
			return nullptr;
		}
		auto AppendPair = [List](const char* Kind, PyObject* Value) -> bool
		{
			if (Value == nullptr)
			{
				return false;
			}
			PyObject* Pair = Py_BuildValue("[sN]", Kind, Value);
			const bool bOk = Pair != nullptr && PyList_Append(List, Pair) == 0;
			Py_XDECREF(Pair);
			return bOk;
		};
		for (const dream::ChainStep& Step : Chain.Steps)
		{
			bool bOk = false;
			switch (Step.Kind)
			{
			case dream::ChainStepKind::Name:
				bOk = AppendPair("name", FromUtf8(Step.Text));
				break;
			case dream::ChainStepKind::Attr:
				bOk = AppendPair("attr", FromUtf8(Step.Text));
				break;
			case dream::ChainStepKind::Sub:
				bOk = AppendPair("sub", Step.bIntKey ? PyLong_FromLongLong(Step.Int) : FromUtf8(Step.Text));
				break;
			case dream::ChainStepKind::Call:
				bOk = AppendPair("call", DreamPy::NewNone());
				break;
			}
			if (!bOk)
			{
				Py_DECREF(List);
				return nullptr;
			}
		}
		if (!AppendPair("prefix", FromUtf8(Chain.Prefix)))
		{
			Py_DECREF(List);
			return nullptr;
		}
		return List;
	}

	const char* ContextName(dream::ChainContext Context)
	{
		switch (Context)
		{
		case dream::ChainContext::Name: return "name";
		case dream::ChainContext::StringArg: return "string_arg";
		default: return "member";
		}
	}

	dream::CompletionKind KindFromName(const char* Name)
	{
		struct FEntry
		{
			const char* Name;
			dream::CompletionKind Kind;
		};
		static const FEntry Table[] = {
			{ "variable", dream::CompletionKind::Variable }, { "function", dream::CompletionKind::Function },
			{ "method", dream::CompletionKind::Method }, { "class", dream::CompletionKind::Class },
			{ "module", dream::CompletionKind::Module }, { "property", dream::CompletionKind::Property },
			{ "field", dream::CompletionKind::Field }, { "constant", dream::CompletionKind::Constant },
			{ "enum_member", dream::CompletionKind::EnumMember }, { "editor_property", dream::CompletionKind::EditorProperty },
			{ "keyword", dream::CompletionKind::Keyword },
		};
		for (const FEntry& Entry : Table)
		{
			if (Name != nullptr && std::strcmp(Name, Entry.Name) == 0)
			{
				return Entry.Kind;
			}
		}
		return dream::CompletionKind::Text;
	}

	/** UTF-8 of a str (CPython's cached form); false for a non-str or a lone surrogate (the error is cleared). */
	bool Utf8Of(PyObject* Object, std::string& Out)
	{
		Out.clear();
		if (Object == nullptr || !PyUnicode_Check(Object))
		{
			return false;
		}
		Py_ssize_t Size = 0;
		const char* Data = PyUnicode_AsUTF8AndSize(Object, &Size);
		if (Data == nullptr)
		{
			PyErr_Clear();
			return false;
		}
		Out.assign(Data, static_cast<size_t>(Size));
		return true;
	}

	FString StrItem(PyObject* Dict, const char* Key)
	{
		FString Out;
		PyObject* Value = PyDict_GetItemString(Dict, Key);
		if (Value != nullptr && PyUnicode_Check(Value) && !DreamPy::ToString(Value, Out))
		{
			PyErr_Clear();
		}
		return Out;
	}

	bool BoolItem(PyObject* Dict, const char* Key)
	{
		PyObject* Value = PyDict_GetItemString(Dict, Key);
		return Value != nullptr && Value == Py_True;
	}

	int64 IntItem(PyObject* Dict, const char* Key, int64 Default)
	{
		PyObject* Value = PyDict_GetItemString(Dict, Key);
		if (Value == nullptr || !DreamPy::IsStrictInt(Value))
		{
			return Default;
		}
		const long long Result = PyLong_AsLongLong(Value);
		if (PyErr_Occurred())
		{
			PyErr_Clear();
			return Default;
		}
		return static_cast<int64>(Result);
	}

	/** One (label, kind, detail, insert, sort) tuple as a Live candidate; false when malformed. */
	bool ParseItem(PyObject* Tuple, dream::ScopeGroup Group, dream::CompletionCandidate& Out)
	{
		if (!PyTuple_Check(Tuple) || PyTuple_GET_SIZE(Tuple) != 5)
		{
			return false;
		}
		std::string Kind;
		if (!Utf8Of(PyTuple_GET_ITEM(Tuple, 0), Out.Label) || Out.Label.empty() || !Utf8Of(PyTuple_GET_ITEM(Tuple, 1), Kind))
		{
			return false;
		}
		Utf8Of(PyTuple_GET_ITEM(Tuple, 2), Out.Detail);
		PyObject* Insert = PyTuple_GET_ITEM(Tuple, 3);
		if (Insert != Py_None)
		{
			Utf8Of(Insert, Out.InsertText);
		}
		Utf8Of(PyTuple_GET_ITEM(Tuple, 4), Out.SortText);
		Out.Kind = KindFromName(Kind.c_str());
		Out.Source = dream::CompletionSource::Live;
		Out.Group = Group;
		return true;
	}

	void ParseItems(PyObject* List, dream::ScopeGroup Group, std::vector<dream::CompletionCandidate>& Out)
	{
		if (List == nullptr || !PyList_Check(List))
		{
			return;
		}
		const Py_ssize_t Count = PyList_GET_SIZE(List);
		Out.reserve(Out.size() + static_cast<size_t>(Count));
		for (Py_ssize_t Index = 0; Index < Count; ++Index)
		{
			dream::CompletionCandidate Candidate;
			if (ParseItem(PyList_GET_ITEM(List, Index), Group, Candidate))
			{
				Out.push_back(MoveTemp(Candidate));
			}
		}
	}

	/** Calls Function(Request) with the GIL held; a new reference to the result dict, or null with OutError. */
	PyObject* CallResolver(const char* Function, PyObject* Request, FString& OutError)
	{
		if (Request == nullptr)
		{
			OutError = DreamPy::FetchExceptionText();
			return nullptr;
		}
		++GResolverCalls;
		PyObject* Callable = GetResolverFunction(Function);
		PyObject* Result = Callable != nullptr ? PyObject_CallOneArg(Callable, Request) : nullptr;
		Py_XDECREF(Callable);
		Py_DECREF(Request);
		if (Result == nullptr)
		{
			OutError = DreamPy::FetchExceptionText();
			return nullptr;
		}
		if (!PyDict_Check(Result))
		{
			OutError = FString::Printf(TEXT("resolver.%hs returned %hs, not a dict"), Function, Py_TYPE(Result)->tp_name);
			Py_DECREF(Result);
			return nullptr;
		}
		return Result;
	}
#endif // WITH_PYTHON
}

uint64 FDreamPython::GetResolverCallCount()
{
	return DreamPythonCompletionPrivate::GResolverCalls.load();
}

bool FDreamPython::ResolverComplete(const FDreamResolverRequest& Request, FDreamResolverResult& OutResult)
{
	using namespace DreamPythonCompletionPrivate;
	check(IsInGameThread());
	OutResult = FDreamResolverResult();
#if WITH_PYTHON
	if (!CanCallResolver(*this))
	{
		return false;
	}
	FDreamScopedGil Gil;
	// The 20 ms breaker (§7.5.3) times the call itself: the wait for a GIL another Python thread holds is not ours.
	const double Start = FPlatformTime::Seconds();
	PyObject* Dict = PyDict_New();
	bool bOk = Dict != nullptr
		&& SetItem(Dict, "chain", BuildChain(Request.Chain))
		&& SetItem(Dict, "context", PyUnicode_FromString(ContextName(Request.Chain.Context)))
		&& SetItem(Dict, "namespace", DreamPy::FromString(Request.Namespace))
		&& SetItem(Dict, "budget_ns", PyLong_FromLongLong(Request.BudgetNs))
		&& SetItem(Dict, "want_members_for", StringList(Request.KnownTokens))
		&& SetItem(Dict, "generation", PyLong_FromUnsignedLong(Request.Generation))
		&& SetItem(Dict, "epoch", PyLong_FromUnsignedLong(Request.Epoch));
	if (bOk && Request.Chain.Context == dream::ChainContext::StringArg)
	{
		bOk = SetItem(Dict, "func", FromUtf8(Request.Chain.Func)) && SetItem(Dict, "index", PyLong_FromLong(Request.Chain.ArgIndex));
	}
	if (!bOk)
	{
		Py_XDECREF(Dict);
		Dict = nullptr;
	}
	PyObject* Result = CallResolver("complete", Dict, OutResult.Error);
	if (Result == nullptr)
	{
		PyErr_Clear();
		OutResult.CallSeconds = FPlatformTime::Seconds() - Start;
		return false;
	}
	PyObject* Token = PyDict_GetItemString(Result, "token");
	if (Token != nullptr && PyUnicode_Check(Token) && !DreamPy::ToString(Token, OutResult.Token))
	{
		PyErr_Clear();
	}
	OutResult.TypeName = StrItem(Result, "type_name");
	OutResult.RootType = StrItem(Result, "root_type");
	OutResult.Breaker = StrItem(Result, "breaker");
	OutResult.Error = StrItem(Result, "error");
	OutResult.bIncomplete = BoolItem(Result, "incomplete");
	OutResult.bRootBound = BoolItem(Result, "root_bound");
	OutResult.bStable = BoolItem(Result, "stable");
	OutResult.bMembersCached = BoolItem(Result, "members_cached");
	OutResult.ElapsedUs = IntItem(Result, "elapsed_us", 0);
	const dream::ScopeGroup Group = Request.Chain.Context == dream::ChainContext::Name ? dream::ScopeGroup::Scope : dream::ScopeGroup::LiveMembers;
	ParseItems(PyDict_GetItemString(Result, "items"), Group, OutResult.Items);
	Py_DECREF(Result);
	PyErr_Clear();
	OutResult.CallSeconds = FPlatformTime::Seconds() - Start;
	return true;
#else
	(void)Request;
	return false;
#endif
}

bool FDreamPython::ResolverPrewarm(const FDreamResolverPrewarmRequest& Request, FDreamResolverPrewarmResult& OutResult)
{
	using namespace DreamPythonCompletionPrivate;
	check(IsInGameThread());
	OutResult = FDreamResolverPrewarmResult();
#if WITH_PYTHON
	if (!CanCallResolver(*this))
	{
		return false;
	}
	FDreamScopedGil Gil;
	PyObject* Dict = PyDict_New();
	const bool bOk = Dict != nullptr
		&& SetItem(Dict, "namespace", DreamPy::FromString(Request.Namespace))
		&& SetItem(Dict, "cursor", PyLong_FromLong(Request.Cursor))
		&& SetItem(Dict, "budget_ns", PyLong_FromLongLong(Request.BudgetNs))
		&& SetItem(Dict, "known_tokens", StringList(Request.KnownTokens))
		&& SetItem(Dict, "generation", PyLong_FromUnsignedLong(Request.Generation))
		&& SetItem(Dict, "epoch", PyLong_FromUnsignedLong(Request.Epoch));
	if (!bOk)
	{
		Py_XDECREF(Dict);
		Dict = nullptr;
	}
	PyObject* Result = CallResolver("prewarm", Dict, OutResult.Error);
	if (Result == nullptr)
	{
		PyErr_Clear();
		return false;
	}
	OutResult.Cursor = static_cast<int32>(IntItem(Result, "cursor", -1));
	OutResult.Count = static_cast<int32>(IntItem(Result, "count", 0));
	OutResult.bIncomplete = BoolItem(Result, "incomplete");
	OutResult.Error = StrItem(Result, "error");

	PyObject* Roots = PyDict_GetItemString(Result, "roots");
	if (Roots != nullptr && PyList_Check(Roots))
	{
		for (Py_ssize_t Index = 0; Index < PyList_GET_SIZE(Roots); ++Index)
		{
			PyObject* Tuple = PyList_GET_ITEM(Roots, Index);
			if (!PyTuple_Check(Tuple) || PyTuple_GET_SIZE(Tuple) != 4)
			{
				continue;
			}
			FDreamResolverRoot Root;
			std::string Kind;
			if (!DreamPy::ToString(PyTuple_GET_ITEM(Tuple, 0), Root.Name) || !Utf8Of(PyTuple_GET_ITEM(Tuple, 1), Kind))
			{
				PyErr_Clear();
				continue;
			}
			Root.Kind = KindFromName(Kind.c_str());
			PyObject* Token = PyTuple_GET_ITEM(Tuple, 2);
			if (Token != Py_None && !DreamPy::ToString(Token, Root.Token))
			{
				PyErr_Clear();
			}
			if (!DreamPy::ToString(PyTuple_GET_ITEM(Tuple, 3), Root.TypeName))
			{
				PyErr_Clear();
			}
			OutResult.Roots.Add(MoveTemp(Root));
		}
	}
	PyObject* Members = PyDict_GetItemString(Result, "members");
	if (Members != nullptr && PyList_Check(Members))
	{
		for (Py_ssize_t Index = 0; Index < PyList_GET_SIZE(Members); ++Index)
		{
			PyObject* Entry = PyList_GET_ITEM(Members, Index);
			if (!PyList_Check(Entry) || PyList_GET_SIZE(Entry) != 4)
			{
				continue;
			}
			FDreamResolverMembers Listing;
			if (!DreamPy::ToString(PyList_GET_ITEM(Entry, 0), Listing.Token) || !DreamPy::ToString(PyList_GET_ITEM(Entry, 1), Listing.TypeName))
			{
				PyErr_Clear();
				continue;
			}
			Listing.bStable = PyList_GET_ITEM(Entry, 2) == Py_True;
			ParseItems(PyList_GET_ITEM(Entry, 3), dream::ScopeGroup::LiveMembers, Listing.Items);
			OutResult.Members.Add(MoveTemp(Listing));
		}
	}
	Py_DECREF(Result);
	PyErr_Clear();
	return true;
#else
	(void)Request;
	return false;
#endif
}

bool FDreamPython::ResolverUnrealNames(int32 Cursor, int64 BudgetNs, FDreamResolverNamesResult& OutResult)
{
	using namespace DreamPythonCompletionPrivate;
	check(IsInGameThread());
	OutResult = FDreamResolverNamesResult();
#if WITH_PYTHON
	if (!CanCallResolver(*this))
	{
		return false;
	}
	FDreamScopedGil Gil;
	PyObject* Dict = PyDict_New();
	const bool bOk = Dict != nullptr
		&& SetItem(Dict, "cursor", PyLong_FromLong(Cursor))
		&& SetItem(Dict, "budget_ns", PyLong_FromLongLong(BudgetNs));
	if (!bOk)
	{
		Py_XDECREF(Dict);
		Dict = nullptr;
	}
	PyObject* Result = CallResolver("unreal_names", Dict, OutResult.Error);
	if (Result == nullptr)
	{
		PyErr_Clear();
		return false;
	}
	OutResult.Cursor = static_cast<int32>(IntItem(Result, "cursor", -1));
	OutResult.Count = static_cast<int32>(IntItem(Result, "count", 0));
	OutResult.Error = StrItem(Result, "error");
	ParseItems(PyDict_GetItemString(Result, "items"), dream::ScopeGroup::LiveMembers, OutResult.Items);
	Py_DECREF(Result);
	PyErr_Clear();
	return true;
#else
	(void)Cursor;
	(void)BudgetNs;
	return false;
#endif
}
