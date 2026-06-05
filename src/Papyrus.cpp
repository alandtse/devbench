#include "Papyrus.h"

#include "Json.h"
#include "MainThread.h"
#include "ToolRegistry.h"

#include <algorithm>
#include <condition_variable>

#include "RE/O/Object.h"
#include "RE/P/PackUnpack.h"  // BindID — the engine's form→Papyrus-object binding helper

// <Windows.h> (via pch) defines a GetObject macro (GDI) that otherwise rewrites
// Variable::GetObject() to GetObjectA. Variable.h undefs it for its own scope only.
#undef GetObject

namespace dvb::Papyrus
{
	namespace
	{
		namespace BSScript = RE::BSScript;

		BSScript::Internal::VirtualMachine* GetVM()
		{
			auto* vm = BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm)
				throw ToolError(503, "Papyrus VM unavailable (game not far enough loaded?)");
			return vm;
		}

		std::string Lower(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}

		std::string Str(const char* a_s) { return a_s ? std::string(a_s) : std::string{}; }

		// A returned Object → { formId, formType, editorId, name } when it is form-backed,
		// else just the class name (aliases / active effects aren't forms). Guarded by walking
		// the script parent chain to "Form" so we never read a non-form pointer as a TESForm.
		json ObjectToJson(BSScript::Internal::VirtualMachine* a_vm, const BSScript::Variable& a_var)
		{
			auto obj = a_var.GetObject();
			if (!obj)
				return json{ { "none", true } };

			auto*             typeInfo = obj->GetTypeInfo();
			const std::string cls = typeInfo ? Str(typeInfo->GetName()) : std::string{};

			bool formBacked = false;
			for (auto* t = typeInfo; t; t = t->GetParent())
				if (Str(t->GetName()) == "Form") {
					formBacked = true;
					break;
				}

			json out{ { "scriptType", cls } };
			if (!formBacked)
				return out;

			RE::VMTypeID typeID{};
			auto*        policy = a_vm->GetObjectHandlePolicy();
			if (policy && a_vm->GetTypeIDForScriptObject(cls.c_str(), typeID)) {
				if (auto* form = static_cast<RE::TESForm*>(policy->GetObjectForHandle(typeID, obj->GetHandle()))) {
					out["formId"] = std::format("0x{:08X}", form->GetFormID());
					out["formType"] = std::format("{}", static_cast<int>(form->GetFormType()));
					out["editorId"] = Str(form->GetFormEditorID());
					if (const char* n = form->GetName(); n && *n)
						out["name"] = n;
				}
			}
			return out;
		}

		// A result Variable → JSON. Scalars map directly; objects resolve to form info;
		// arrays map element-wise; none becomes null (returnedType reports the real type).
		json VariableToJson(BSScript::Internal::VirtualMachine* a_vm, const BSScript::Variable& a_var)
		{
			if (a_var.IsBool())
				return a_var.GetBool();
			if (a_var.IsInt())
				return a_var.GetSInt();
			if (a_var.IsFloat())
				return a_var.GetFloat();
			if (a_var.IsString())
				return std::string(a_var.GetString());
			if (a_var.IsObject())
				return ObjectToJson(a_vm, a_var);
			if (a_var.IsArray()) {
				json out = json::array();
				if (auto arr = a_var.GetArray())
					for (RE::BSScript::Array::size_type i = 0; i < arr->size(); ++i)
						out.push_back(VariableToJson(a_vm, (*arr)[i]));
				return out;
			}
			return nullptr;  // none — see returnedType
		}

		// Parse a "0x.."-style hex FormID (after an optional 0x prefix), or nullptr.
		RE::TESForm* FormByHex(const std::string& a_hex)
		{
			try {
				return RE::TESForm::LookupByID(static_cast<RE::FormID>(std::stoul(a_hex, nullptr, 16)));
			} catch (...) {
				return nullptr;
			}
		}

		// Resolve a form reference to a TESForm, or nullptr. An explicit `0x..` is a FormID;
		// otherwise try EditorID first (so an all-hex EditorID isn't misread as a FormID), then
		// fall back to a bare hex FormID (the `14` shorthand).
		RE::TESForm* LookupForm(const std::string& a_ref)
		{
			if (a_ref.size() > 2 && a_ref[0] == '0' && (a_ref[1] == 'x' || a_ref[1] == 'X'))
				return FormByHex(a_ref.substr(2));
			if (auto* form = RE::TESForm::LookupByEditorID(a_ref))
				return form;
			return FormByHex(a_ref);
		}

		// Find or create+bind a script Object of `a_className` for `a_form` — the engine's own
		// form→Papyrus binding (FindBoundObject → CreateObject → BindID; see CommonLib PackUnpack).
		// Dispatching/packing on a form that isn't bound is what CTD'd. Null if it can't bind.
		RE::BSTSmartPointer<BSScript::Object> BindFormObject(BSScript::Internal::VirtualMachine* a_vm, RE::TESForm* a_form, const char* a_className)
		{
			RE::BSTSmartPointer<BSScript::Object> obj;
			auto*                                 policy = a_vm->GetObjectHandlePolicy();
			if (!policy || !a_className)
				return obj;
			const auto typeID = static_cast<RE::VMTypeID>(a_form->GetFormType());
			const auto handle = policy->GetHandleForObject(typeID, a_form);
			if (handle == policy->EmptyHandle())
				return obj;
			if (!a_vm->FindBoundObject(handle, a_className, obj) || !obj)
				if (a_vm->CreateObject(a_className, obj) && obj)
					RE::BSScript::BindID(obj, a_form, typeID);
			return obj;
		}

		BSScript::Variable JsonToVariable(BSScript::Internal::VirtualMachine* a_vm, const json& a_arg, const BSScript::TypeInfo* a_paramType = nullptr);

		// Pack a form into a Variable typed as `a_paramType` expects. PackHandle would type the
		// form as its *native* class, which a param declared as a base class (Form/ObjectReference)
		// rejects; binding to the param's class lets the upcast bind. Native fallback otherwise.
		void PackFormToParam(BSScript::Internal::VirtualMachine* a_vm, RE::TESForm* a_form, const BSScript::TypeInfo& a_paramType, BSScript::Variable& a_out)
		{
			if (a_paramType.IsObject())
				if (auto* paramClass = a_paramType.GetTypeInfo(); paramClass)
					if (auto obj = BindFormObject(a_vm, a_form, paramClass->GetName())) {
						a_out.SetObject(obj, a_paramType.GetRawType());
						return;
					}
			BSScript::PackHandle(&a_out, a_form, static_cast<RE::VMTypeID>(a_form->GetFormType()));
		}

		// Build a typed Papyrus Array from a JSON array of scalars. The element type comes from
		// the contents (a typed Papyrus array can't be inferred from an empty list, so empty is
		// rejected) and every element must share one kind — a mixed array would otherwise be packed
		// as the first element's type and mis-typed silently. Int promotes to Float if any element
		// is fractional.
		BSScript::Variable JsonArrayToVariable(BSScript::Internal::VirtualMachine* a_vm, const json& a_arr)
		{
			using Raw = BSScript::TypeInfo::RawType;
			// kind: bool / number / string — the homogeneity classes (int vs float both number)
			auto kind = [](const json& e) -> int { return e.is_boolean() ? 0 : e.is_number() ? 1 :
				                                                           e.is_string()     ? 2 :
				                                                                               -1; };
			if (a_arr.empty())
				throw ToolError(400, "papyrus call: empty array args can't be typed — pass a non-empty array");
			const int k0 = kind(a_arr.front());
			if (k0 < 0)
				throw ToolError(400, "papyrus call: array args support only bool/number/string elements");
			bool anyFloat = false;
			for (const auto& e : a_arr) {
				if (kind(e) != k0)
					throw ToolError(400, "papyrus call: array args must be homogeneous (all bool, all number, or all string)");
				anyFloat = anyFloat || e.is_number_float();
			}
			const Raw elem = (k0 == 0) ? Raw::kBool : (k0 == 2) ? Raw::kString :
			                                                      (anyFloat ? Raw::kFloat : Raw::kInt);

			RE::BSTSmartPointer<BSScript::Array> array;
			if (!a_vm->CreateArray(BSScript::TypeInfo(elem), static_cast<std::uint32_t>(a_arr.size()), array) || !array)
				throw ToolError(500, "papyrus call: could not allocate a Papyrus array");
			for (std::uint32_t i = 0; i < a_arr.size(); ++i)
				(*array)[i] = JsonToVariable(a_vm, a_arr[i]);

			BSScript::Variable v;
			v.SetArray(std::move(array));
			return v;
		}

		// One JSON arg → one Variable. Scalars are direct; { "form": "0x14" | "EditorID" } resolves
		// a TESForm and packs it to the declared param type (a_paramType) so base-typed params bind,
		// else to its native type; a JSON array becomes a typed Papyrus array. Touches the VM (form
		// handles / array alloc) — call on the main thread.
		BSScript::Variable JsonToVariable(BSScript::Internal::VirtualMachine* a_vm, const json& a_arg, const BSScript::TypeInfo* a_paramType)
		{
			BSScript::Variable v;
			if (a_arg.is_boolean()) {
				v.SetBool(a_arg.get<bool>());
			} else if (a_arg.is_number_float()) {
				v.SetFloat(static_cast<float>(a_arg.get<double>()));
			} else if (a_arg.is_number_integer() || a_arg.is_number_unsigned()) {
				v.SetSInt(a_arg.get<std::int32_t>());
			} else if (a_arg.is_string()) {
				v.SetString(a_arg.get<std::string>());
			} else if (a_arg.is_array()) {
				return JsonArrayToVariable(a_vm, a_arg);
			} else if (a_arg.is_object() && a_arg.contains("form")) {
				const std::string ref = a_arg["form"].get<std::string>();
				RE::TESForm*      form = LookupForm(ref);
				if (!form)
					throw ToolError(400, std::format("papyrus call: form '{}' not found (formId hex or EditorID)", ref));
				if (a_paramType)
					PackFormToParam(a_vm, form, *a_paramType, v);
				else
					BSScript::PackHandle(&v, form, static_cast<RE::VMTypeID>(form->GetFormType()));
			} else {
				throw ToolError(400, "papyrus call: each arg must be a bool, number, string, array, or { \"form\": \"0x.. | EditorID\" }");
			}
			return v;
		}

		// Find a function by name on a type, walking the parent chain for member functions
		// (globals/statics are not inherited). Case-insensitive (Papyrus names are). For resolving
		// param types so form args can be packed to the declared (possibly base) param class.
		const BSScript::IFunction* FindFunction(BSScript::ObjectTypeInfo* a_type, std::string_view a_name, bool a_global)
		{
			const std::string needle = Lower(std::string(a_name));
			for (auto* t = a_type; t; t = t->GetParent()) {
				const std::uint32_t n = a_global ? t->GetNumGlobalFuncs() : t->GetNumMemberFuncs();
				for (std::uint32_t i = 0; i < n; ++i) {
					const auto& f = a_global ? t->GetGlobalFuncIter()[i].func : t->GetMemberFuncIter()[i].func;
					if (f && f->GetName().c_str() && Lower(std::string(f->GetName().c_str())) == needle)
						return f.get();
				}
				if (a_global)
					break;
			}
			return nullptr;
		}

		json DescribeFunction(const BSScript::IFunction* a_fn)
		{
			json params = json::array();
			for (std::uint32_t p = 0; p < a_fn->GetParamCount(); ++p) {
				RE::BSFixedString  name;
				BSScript::TypeInfo type;
				a_fn->GetParam(p, name, type);
				params.push_back(json{ { "name", Str(name.c_str()) }, { "type", type.TypeAsString() } });
			}
			return json{
				{ "name", Str(a_fn->GetName().c_str()) },
				{ "returnType", a_fn->GetReturnType().TypeAsString() },
				{ "params", std::move(params) },
				{ "native", a_fn->GetIsNative() },
			};
		}

		// IFunctionArguments built from a runtime list — MakeFunctionArguments is compile-time
		// only, so 'call' needs its own that copies pre-built Variables into the VM's scrap array.
		class RuntimeArgs : public BSScript::IFunctionArguments
		{
		public:
			std::vector<BSScript::Variable> args;

			bool operator()(RE::BSScrapArray<BSScript::Variable>& a_dst) const override
			{
				using size_type = RE::BSScrapArray<BSScript::Variable>::size_type;
				a_dst.resize(static_cast<size_type>(args.size()));
				for (size_type i = 0; i < a_dst.size(); ++i)
					a_dst[i] = args[i];
				return true;
			}
		};

		// Shared completion state. Held by a shared_ptr so a late VM callback (after a timeout
		// already returned) writes into live memory, never a freed functor's fields.
		struct CallState
		{
			std::mutex              m;
			std::condition_variable cv;
			bool                    done = false;
			int                     status = 400;  // HTTP status to surface when `error` is set
			std::string             error;         // non-empty → arg-build / bind / dispatch failed
			BSScript::Variable      result;
		};

		class CallFunctor : public BSScript::IStackCallbackFunctor
		{
		public:
			explicit CallFunctor(std::shared_ptr<CallState> a_state) :
				_state(std::move(a_state)) {}

			void operator()(BSScript::Variable a_result) override
			{
				std::lock_guard<std::mutex> lk(_state->m);
				_state->result = a_result;
				_state->done = true;
				_state->cv.notify_all();
			}

			bool CanSave() const override { return false; }
			void SetObject(const RE::BSTSmartPointer<BSScript::Object>&) override {}

		private:
			std::shared_ptr<CallState> _state;
		};

		json HandleList(const json& a_args)
		{
			const std::string filter = a_args.value("filter", std::string{});
			const int         limit = a_args.value("limit", 200);
			return MainThread::RunAndWait([filter, limit]() -> json {
				auto*                    vm = GetVM();
				const std::string        needle = Lower(filter);
				std::vector<std::string> names;
				int                      total = 0;
				{
					RE::BSSpinLockGuard lock(vm->typeInfoLock);
					for (auto& entry : vm->objectTypeMap) {
						std::string name = Str(entry.first.c_str());
						if (name.empty())
							continue;
						if (!needle.empty() && Lower(name).find(needle) == std::string::npos)
							continue;
						++total;
						if (static_cast<int>(names.size()) < limit)
							names.push_back(std::move(name));
					}
				}
				std::sort(names.begin(), names.end());
				return json{
					{ "total", total },
					{ "returned", static_cast<int>(names.size()) },
					{ "truncated", total > static_cast<int>(names.size()) },
					{ "scripts", std::move(names) },
				};
			});
		}

		json HandleDescribe(const json& a_args)
		{
			const std::string script = a_args.value("script", std::string{});
			if (script.empty())
				throw ToolError(400, "papyrus describe requires 'script' (class name)");
			return MainThread::RunAndWait([script]() -> json {
				auto*                                         vm = GetVM();
				RE::BSTSmartPointer<BSScript::ObjectTypeInfo> type;
				if (!vm->GetScriptObjectType1(script.c_str(), type) || !type)
					throw ToolError(404, std::format("unknown script class '{}'", script));

				json globals = json::array();
				for (std::uint32_t i = 0; i < type->GetNumGlobalFuncs(); ++i)
					if (auto& fn = type->GetGlobalFuncIter()[i].func)
						globals.push_back(DescribeFunction(fn.get()));

				json members = json::array();
				for (std::uint32_t i = 0; i < type->GetNumMemberFuncs(); ++i)
					if (auto& fn = type->GetMemberFuncIter()[i].func)
						members.push_back(DescribeFunction(fn.get()));

				json props = json::array();
				for (std::uint32_t i = 0; i < type->GetNumProperties(); ++i)
					props.push_back(Str(type->GetPropertyIter()[i].name.c_str()));

				const auto* parent = type->GetParent();
				json        out{
					{ "name", Str(type->GetName()) },
					{ "globalFunctions", std::move(globals) },
					{ "memberFunctions", std::move(members) },
					{ "properties", std::move(props) },
				};
				out["parent"] = parent ? json(Str(parent->GetName())) : json(nullptr);
				return out;
			});
		}

		// Resolve a 'self' selector to a *bound* script Object for member dispatch. "selected"
		// uses the console-selected ref (set by `prid`, the crosshair, or a click); otherwise a
		// { "form": ".." } / formId / EditorID targets any form. A member call needs the form
		// bound to a script object of `a_className` first — find the existing binding or create +
		// bind one (the same FindBoundObject → CreateObject → BindID dance the engine itself runs
		// for every form it passes into Papyrus; see CommonLib PackUnpack). Skipping the bind and
		// dispatching on a bare handle is what CTD'd. Main thread only.
		bool ResolveSelf(BSScript::Internal::VirtualMachine* a_vm, const json& a_self, const RE::BSFixedString& a_className,
			RE::BSTSmartPointer<BSScript::Object>& a_out, std::string& a_err)
		{
			RE::TESForm* form = nullptr;
			if (a_self.is_string() && a_self.get<std::string>() == "selected") {
				if (auto sel = RE::Console::GetSelectedRef())
					form = sel.get();
				if (!form) {
					a_err = "self='selected' but no console-selected ref (use `prid <id>`, the crosshair, or click one)";
					return false;
				}
			} else {
				const std::string ref =
					a_self.is_string() ? a_self.get<std::string>() : (a_self.is_object() && a_self.contains("form") ? a_self["form"].get<std::string>() : std::string{});
				if (ref.empty()) {
					a_err = "self must be \"selected\" or { \"form\": \"0x.. | EditorID\" }";
					return false;
				}
				form = LookupForm(ref);
				if (!form) {
					a_err = std::format("self form '{}' not found", ref);
					return false;
				}
			}

			// Bind to the form's NATIVE script type, not the caller's `script`: BindID's
			// HandleIsType check is exact, so binding e.g. an Actor handle to a parent class
			// ("ObjectReference") fails silently. The bound object's hierarchy still satisfies the
			// function (an Actor has ObjectReference.GetDistance). Fall back to the caller's class.
			RE::BSFixedString                             bindClass = a_className;
			RE::BSTSmartPointer<BSScript::ObjectTypeInfo> nativeType;
			if (a_vm->GetScriptObjectType(static_cast<RE::VMTypeID>(form->GetFormType()), nativeType) && nativeType && nativeType->GetName())
				bindClass = nativeType->GetName();

			a_out = BindFormObject(a_vm, form, bindClass.c_str());
			if (!a_out) {
				a_err = std::format("could not bind form 0x{:08X} to a '{}' object", form->GetFormID(), bindClass.c_str());
				return false;
			}
			return true;
		}

		json HandleCall(const json& a_args)
		{
			const std::string script = a_args.value("script", std::string{});
			const std::string function = a_args.value("function", std::string{});
			if (script.empty() || function.empty())
				throw ToolError(400, "papyrus call requires 'script' and 'function'");
			const int  timeoutMs = a_args.value("timeoutMs", 3000);
			const json argsJson = a_args.contains("args") ? a_args["args"] : json::array();
			if (!argsJson.is_array())
				throw ToolError(400, "papyrus call: 'args' must be an array");
			const json selfJson = a_args.contains("self") ? a_args["self"] : json();
			const bool hasSelf = !selfJson.is_null();

			auto* task = SKSE::GetTaskInterface();
			if (!task)
				throw ToolError(500, "SKSE TaskInterface unavailable");

			auto                    state = std::make_shared<CallState>();
			const RE::BSFixedString cls(script.c_str());
			const RE::BSFixedString fn(function.c_str());

			// Everything that touches the VM (form/array packing, handle bind, dispatch) runs on
			// the main thread; the result arrives async on the VM tasklet thread via CallFunctor.
			// Arg-build / bind / dispatch failures are reported back through state, preserving the
			// status so an internal fault (503) isn't misreported to the caller as a 400.
			auto fail = [state](int a_status, std::string a_msg) {
				std::lock_guard<std::mutex> lk(state->m);
				state->status = a_status;
				state->error = std::move(a_msg);
				state->done = true;
				state->cv.notify_all();
			};
			task->AddTask([cls, fn, argsJson, selfJson, hasSelf, state, fail]() {
				auto* vm = BSScript::Internal::VirtualMachine::GetSingleton();
				if (!vm) {
					fail(503, "Papyrus VM unavailable");
					return;
				}

				// Resolve self first (member) — the bound object's type is where we look up the
				// function, and where args bind. For a global, the script type holds the function.
				RE::BSTSmartPointer<BSScript::Object> selfObj;
				if (hasSelf) {
					std::string err;
					if (!ResolveSelf(vm, selfJson, cls, selfObj, err)) {
						fail(400, err);
						return;
					}
				}
				RE::BSTSmartPointer<BSScript::ObjectTypeInfo> scriptType;  // keeps a global's type alive
				BSScript::ObjectTypeInfo*                     fnType = nullptr;
				if (hasSelf)
					fnType = selfObj->GetTypeInfo();
				else if (vm->GetScriptObjectType(cls, scriptType) && scriptType)
					fnType = scriptType.get();

				// Resolve the function up front — REQUIRED, not just for param types: a
				// `DispatchStaticCall` to a non-existent global function null-derefs in the VM (a
				// hard CTD, confirmed live), so a function that can't be resolved must fail cleanly
				// here and never reach the dispatch.
				const BSScript::IFunction* ifn = fnType ? FindFunction(fnType, std::string_view(fn.c_str() ? fn.c_str() : ""), !hasSelf) : nullptr;
				if (!ifn) {
					fail(404, std::format("no such {} function '{}' on script '{}'", hasSelf ? "member" : "global/native", fn.c_str() ? fn.c_str() : "", cls.c_str() ? cls.c_str() : ""));
					return;
				}
				// Declared param types so a form arg can be packed to a base-typed param.
				std::vector<BSScript::TypeInfo> paramTypes;
				for (std::uint32_t p = 0; p < ifn->GetParamCount(); ++p) {
					RE::BSFixedString  pn;
					BSScript::TypeInfo pt;
					ifn->GetParam(p, pn, pt);
					paramTypes.push_back(pt);
				}

				auto* rawArgs = new RuntimeArgs();
				try {
					std::size_t i = 0;
					for (const auto& a : argsJson) {
						const BSScript::TypeInfo* pt = (i < paramTypes.size()) ? &paramTypes[i] : nullptr;
						rawArgs->args.push_back(JsonToVariable(vm, a, pt));
						++i;
					}
				} catch (const ToolError& e) {
					delete rawArgs;
					fail(e.code, e.what());
					return;
				} catch (const std::exception& e) {
					delete rawArgs;
					fail(400, e.what());
					return;
				}

				RE::BSTSmartPointer<BSScript::IStackCallbackFunctor> cb(new CallFunctor(state));
				const bool                                           ok = hasSelf ? vm->DispatchMethodCall(selfObj, fn, rawArgs, cb) : vm->DispatchStaticCall(cls, fn, rawArgs, cb);
				if (!ok)
					fail(400, hasSelf ? "method dispatch refused — unknown function, wrong arg count, or not a member of that object's script" : "dispatch refused — unknown function, wrong arg count, or not a global/native function");
			});

			std::unique_lock<std::mutex> lk(state->m);
			const bool                   completed = state->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
				[&] { return state->done; });
			if (!completed)
				throw ToolError(504, std::format("papyrus call '{}.{}' did not complete within {}ms (latent call or VM stalled?)", script, function, timeoutMs));
			if (!state->error.empty())
				throw ToolError(state->status, std::format("papyrus call '{}.{}': {}", script, function, state->error));

			BSScript::Variable result = state->result;
			lk.unlock();

			return MainThread::RunAndWait([result]() -> json {
				auto* vm = GetVM();
				return json{
					{ "called", true },
					{ "returned", VariableToJson(vm, result) },
					{ "returnedType", result.GetType().TypeAsString() },
				};
			});
		}
	}

	json Handle(const json& a_args, const ToolContext&)
	{
		const std::string action = a_args.value("action", std::string("list"));
		if (action == "list")
			return HandleList(a_args);
		if (action == "describe")
			return HandleDescribe(a_args);
		if (action == "call")
			return HandleCall(a_args);
		throw ToolError(400, std::format("unknown action '{}' (list|describe|call)", action));
	}
}
