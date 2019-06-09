//
//  MiniscriptTypes.cpp
//  MiniScript
//
//  Created by Joe Strout on 5/31/18.
//  Copyright © 2018 Joe Strout. All rights reserved.
//

#include "MiniscriptTypes.h"
#include "MiniscriptErrors.h"
#include "MiniscriptIntrinsics.h"
#include "MiniscriptTAC.h"
#include "UnitTest.h"
#include "SplitJoin.h"

#include <iostream>
#include <math.h>

namespace MiniScript {
	
	long Value::maxStringSize = 0xFFFFFF;		// about 16MB
	long Value::maxListSize   = 0xFFFFFF;		// about 16M elements

	Value Value::zero(0.0);
	Value Value::one(1.0);
	Value Value::emptyString("");
	Value Value::magicIsA("__isa");
	Value Value::null;
	Value Value::keyString("key");
	Value Value::valueString("value");
	Value Value::implicitResult = Value::Var("_");

	String ToString(ValueType type) {
		switch (type) {
			case ValueType::Null:	return "Null";
			case ValueType::Number: return "Number";
			case ValueType::Temp: return "Temp";
			case ValueType::String: return "String";
			case ValueType::List: return "List";
			case ValueType::Map: return "Map";
			case ValueType::Function: return "Function";
			case ValueType::Var: return "Var";
			case ValueType::SeqElem: return "SeqElem";
		}
		return "Unknown";
	}

	String Value::ToString() {
		if (type == ValueType::Number) {
			// Convert number to string in the standard Miniscript way.
			double value = data.number;
			if (fmod(value, 1.0) == 0.0) {
				return String::Format(value, "%.0f");
			} else if (value > 1E10 || value < -1E10 || (value < 1E-6 && value > -1E-6)) {
				// very large/small numbers in exponential form
				return String::Format(value, "%.6E");
			} else {
				// all others in decimal form, with 1-6 digits past the decimal point
				String s = String::Format(value, "%.6f");
				long i = s.LengthB() - 1;
				while (i > 1 && s[i] == '0' && s[i-1] != '.') i--;
				if (i+1 < s.LengthB()) s = s.SubstringB(0, i+1);
				return s;
			}
		}
		if (type == ValueType::String) { retain(); return String((StringStorage*)data.ref); }
		if (type == ValueType::List) return CodeForm(3);
		if (type == ValueType::Map) return CodeForm(3);
		if (type == ValueType::Var) { retain(); return String((StringStorage*)data.ref); }
		if (type == ValueType::Temp) return String("_") + String::Format((int)data.tempNum);
		if (type == ValueType::Function) {
			String s("FUNCTION(");
			FunctionStorage *fs = (FunctionStorage*)data.ref;
			for (long i=0; i < fs->parameters.Count(); i++) {
				if (i > 0) s += ", ";
				s += fs->parameters[i].name;
				if (not fs->parameters[i].defaultValue.IsNull()) s += fs->parameters[i].defaultValue.ToString();
			}
			return s + ")";
		}
		if (type == ValueType::SeqElem) {
			SeqElemStorage *se = (SeqElemStorage*)data.ref;
			return se->sequence.ToString() + "[" + se->index.ToString() + "]";
		}
		return String();
	}

	String Value::CodeForm(int recursionLimit) {
		switch (type) {

			case ValueType::Null:
				return "null";
				
			case ValueType::Number:
				return ToString();

			case ValueType::String:
			{
				String temp((StringStorage*)data.ref);
				String result = "\"" + temp.Replace("\"", "\"\"") + "\"";
//				temp.forget();
				return result;
			} break;

			case ValueType::List:
			{
				if (recursionLimit <= 0) return "[...]";
				List<Value> list((ListStorage<Value>*)data.ref);
				long count = list.Count();
				if (count == 0) {
//					list.forget();
					 return "[]";
				}
				List<String> strs(count);
				for (long i=0; i<count; i++) strs.Add(list[i].CodeForm(recursionLimit-1));
				String result = String("[") + Join(", ", strs) + "]";
//				list.forget();
				return result;
			} break;

			case ValueType::Map:
			{
				if (recursionLimit == 0) return "{...}";
				ValueDict map((ValueDictStorage*)data.ref);
				List<String> strs = List<String>(map.Count());
				for (ValueDictIterator kv = map.GetIterator(); not kv.Done(); kv.Next()) {
					strs.Add(kv.Key().CodeForm(recursionLimit-1) + ": " + kv.Value().CodeForm(recursionLimit-1));
				}
//				map.forget();
				return String("{") + Join(", ", strs) + String("}");
			}
				
			case ValueType::Var:
			case ValueType::Temp:
				return ToString();

			default:
				return String();
		}
	}
	
	Value Value::Val(Context *context, ValueDict *outFoundInMap) const {
		switch (type) {
			case ValueType::Temp:
				return context->GetTemp(data.tempNum);
			case ValueType::Var:
			{
				String ident((StringStorage*)(data.ref));
				Value result = context->GetVar(ident);
//				ident.forget();
				return result;
			} break;
			case ValueType::SeqElem:
			{
				// There is a TAC opcode for looking this up.  But, when chaining
				// lookups, it's darned convenient to just ask each step to get its value.
				// SO:
				if (data.ref == nullptr) return Value::null;
				Value sequence = ((SeqElemStorage*)(data.ref))->sequence;
				Value index = ((SeqElemStorage*)(data.ref))->index;
				Value idxVal = index.IsNull() ? null : index.Val(context);
				if (idxVal.type == ValueType::String) {
					String idxStr((StringStorage*)(idxVal.data.ref));
					Value result = Resolve(sequence, idxStr, context, outFoundInMap);
//					idxStr.forget();
					return result;
				}
				// Ok, we're searching for something that's not a string;
				// this can only be done in maps, lists, and strings (and lists/strings, only with a numeric index).
				Value baseVal = sequence.Val(context);
				if (baseVal.type == ValueType::Map) {
					Value result = null;
					// Keep walking the "__isa" chain until we find the value, or can go no further.
					while (baseVal.type == ValueType::Map) {
						if (idxVal.IsNull()) throw KeyException("null");
						ValueDict baseDict((ValueDictStorage*)(baseVal.data.ref));
						if (baseDict.Get(idxVal, &result)) {
//							baseDict.forget();
							return result;
						}
						if (not baseDict.Get(Value::magicIsA, &baseVal)) {
//							baseDict.forget();
							throw KeyException(idxVal.ToString());
						}
//						baseDict.forget();
						baseVal = baseVal.Val(context);	// ToDo: is this really needed?
					}
				} else if (baseVal.type == ValueType::List and idxVal.type == ValueType::Number) {
					ValueList baseLst((ValueListStorage*)(baseVal.data.ref));
					Value result = baseLst.Item((long)(idxVal.data.number));
//					baseLst.forget();
					return result;
				} else if (baseVal.type == ValueType::String and idxVal.type == ValueType::Number) {
					String baseStr((StringStorage*)(baseVal.data.ref));
					long len = baseStr.Length();
					long i = (long)idxVal.data.number;
					if (i < 0) i += len;
					if (i < 0 or i >= len) {
//						baseStr.forget();
						throw IndexException(String("Index Error (string index ") + i + " out of range");
					}
					Value result = baseStr.Substring(i, 1);
					return result;
				}
				
				throw TypeException("Type Exception: can't index into this type");

			} break;
			default:
				// Most types evaluate to themselves
				return *this;
		}
	}
	
	long Value::IntValue() {
		if (type == ValueType::Number) return data.number;
		return 0;
	}
	
	bool Value::BoolValue() {
		switch (type) {
			case ValueType::Number:
				// Any nonzero value is considered true, when treated as a bool.
				return data.number != 0;
				
			case ValueType::String:
			{
				// Any nonempty string is true.
				String s((StringStorage*)(data.ref));
				bool result = not s.empty();
//				s.forget();
				return result;
			}
				
			case ValueType::List:
			{
				// Any nonempty list is true.
				ValueList l((ListStorage<Value>*)(data.ref));
				bool result = (l.Count() > 0);
//				l.forget();
				return result;
			}

			case ValueType::Map:
			{
				// Any nonempty map is true.
				ValueDict d((DictionaryStorage<Value, Value>*)(data.ref));
				bool result = not d.empty();
//				d.forget();
				return result;
			}

			default:
				return false;
		}

	}

	/// Evaluate each of our contained elements, and if any of those is a variable
	/// or temp, then resolve them now.  CAUTION: do not mutate the original list
	/// or map!  We may need it in its original form on future iterations.
	Value Value::FullEval(Context *context) {
		if (type == ValueType::List) {
			ValueList result;
			bool gotNewResult = false;
			ValueList src((ValueListStorage*)(data.ref));
			long count = src.Count();
			for (long i=0; i<count; i++) {
				bool copied = false;
				if (src[i].type == ValueType::Temp or src[i].type == ValueType::Var) {
					Value newVal = src[i].Val(context);
					if (newVal != src[i]) {
						// OK, something changed, so we're going to need a new copy of the list.
						if (not gotNewResult) {
							for (long j = 0; j < i; j++) result.Add(src[i]);
							gotNewResult = true;
						}
						result.Add(newVal);
						copied = true;
					}
				}
				if (not copied and gotNewResult) {
					// No change for this value; but we have new results to return, so copy it as-is
					result.Add(src[i]);
				}
			}
//			src.forget();
			if (gotNewResult) return result;
			return *this;
		} else if (type == ValueType::Map) {
			ValueDict src((ValueDictStorage*)(data.ref));
			ValueDict result;
			bool gotNewResult = false;
			for (ValueDictIterator iter=src.GetIterator(); not iter.Done(); iter.Next()) {
				Value key = iter.Key();
				Value val = iter.Value();
				bool copied = false;
				if (key.type == ValueType::Temp or key.type == ValueType::Var
					or val.type == ValueType::Temp or val.type == ValueType::Var) {
					Value newKey = key.Val(context);
					Value newVal = val.Val(context);
					if (newKey != key or newVal != val) {
						// OK, something changed, so we're going to need a new copy of the map.
						if (not gotNewResult) {
							for (ValueDictIterator iter2=src.GetIterator(); iter2 != iter; iter2.Next()) {
								result.SetValue(iter2.Key(), iter2.Value());
							}
							gotNewResult = true;
						}
						result.SetValue(newKey, newVal);
						copied = true;
					}
				}
				if (not copied and not gotNewResult) {
					// No change for this value; but we have new results to return, so copy it as-is
					result.SetValue(key, val);
				}
			}
//			src.forget();
			if (gotNewResult) return result;
			return *this;
		} else if (IsNull()) {
			return Value::null;
		} else return Val(context);
	}


	/// Create a copy of this value, evaluating sub-values as we go.
	/// This is used when a list or map literal appears in the source, to
	/// ensure that each time that code executes, we get a new, distinct
	/// mutable object, rather than the same object referenced each time.
	/// (Used with literals, and in the case of a Map, it's also used with 'new'.)
	Value Value::EvalCopy(Context *context) {
		if (type == ValueType::List) {
			ValueList src((ValueListStorage*)(data.ref));
			long count = src.Count();
			ValueList result(count);
			for (long i=0; i<count; i++) result.Add(src[i].Val(context));
//			src.forget();
			return result;
		} else if (type == ValueType::Map) {
			ValueDict src((ValueDictStorage*)(data.ref));
			ValueDict result;
			for (ValueDictIterator iter=src.GetIterator(); not iter.Done(); iter.Next()) {
				Value key = iter.Key();
				Value val = iter.Value();
				if (key.type == ValueType::Temp or key.type == ValueType::Var) key = key.Val(context);
				if (val.type == ValueType::Temp or val.type == ValueType::Var) val = val.Val(context);
				result.SetValue(key, val);
			}
//			src.forget();
			return result;
		} else if (IsNull()) {
			return Value::null;
		} else return Val(context);
	}

	
	/// <summary>
	/// Set an element associated with the given index within this Value.
	/// </summary>
	/// <param name="index">index/key for the value to set</param>
	/// <param name="value">value to set</param>
	void Value::SetElem(Value index, Value value) {
		if (type == ValueType::List) {
			long i = index.IntValue();
			ValueList list = GetList();
			if (i < 0) i += list.Count();
			if (i < 0 or i >= list.Count()) {
//				list.forget();
				throw IndexException(String("Index Error (list index " + index.ToString() + " out of range)"));
			}
			list[i] = value;
//			list.forget();
		} else if (type == ValueType::Map) {
			ValueDict dict = GetDict();
			dict.SetValue(index, value);
//			dict.forget();
		}
	}

	
	/// <summary>
	/// Look up the given identifier in the given sequence, walking the type chain
	/// until we either find it, or fail.
	/// </summary>
	/// <param name="sequence">Sequence (object) to look in.</param>
	/// <param name="identifier">Identifier to look for.</param>
	/// <param name="context">Context.</param>
	/// <param name="outFoundInMap">Output parameter: map the value was found in.</param>
	Value Value::Resolve(Value sequence, String identifier, Context *context, ValueDict *outFoundInMap) {
		bool includeMapType = true;
		while (not sequence.IsNull()) {
			if (sequence.type == ValueType::Temp or sequence.type == ValueType::Var) sequence = sequence.Val(context);
			if (sequence.type == ValueType::Map) {
				// If the map contains this identifier, return its value.
				Value result;
				ValueDict d = sequence.GetDict();
				if (d.Get(Value(identifier), &result)) {
//					d.forget();
					if (outFoundInMap) *outFoundInMap = d;
					return result;
				}
				// Otherwise, if we have an __isa, try that next
				if (not d.Get(Value::magicIsA, &sequence)) {
					// ...and if we don't have an __isa, try the generic map type if allowed
					if (!includeMapType) throw KeyException(identifier);
					sequence = Intrinsics::MapType();
					includeMapType = false;
				}
//				d.forget();
			} else if (sequence.type == ValueType::List) {
				sequence = context->vm->listType;
				if (sequence.IsNull()) sequence = Intrinsics::ListType();
				includeMapType = false;
			} else if (sequence.type == ValueType::String) {
				sequence = context->vm->stringType;
				if (sequence.IsNull()) sequence = Intrinsics::StringType();
				includeMapType = false;
			} else if (sequence.type == ValueType::Map) {
				sequence = context->vm->mapType;
				if (sequence.IsNull()) sequence = Intrinsics::MapType();
				includeMapType = false;
			} else if (sequence.type == ValueType::Number) {
				sequence = context->vm->numberType;
				if (sequence.IsNull()) sequence = Intrinsics::NumberType();
				includeMapType = false;
			} else if (sequence.type == ValueType::Function) {
				sequence = Intrinsics::FunctionType();
				includeMapType = false;
			} else {
				throw TypeException("Type Error (while attempting to look up " + identifier + ")");
			}
		}
		return null;

	}
	
	/// <summary>
	/// Determine whether this value is the given type (or some subclass)
	/// in the context of the given virtual machine.
	/// </summary>
	bool Value::IsA(Value type, Machine *vm) {
		switch (this->type) {
			case ValueType::Number:
				return type == vm->numberType;
				
			case ValueType::String:
				return type == vm->stringType;
				
			case ValueType::List:
				return type == vm->listType;
				
			case ValueType::Function:
				return type == vm->functionType;
				
			case ValueType::Map:
			{
				// if the given type is the map base type, we're definitely that
				if (type == vm->mapType) return true;
				// otherwise, walk the __isa chain
				ValueDict d = GetDict();
				Value p;
				if (!d.Get(magicIsA, &p)) return false;
				while (true) {
					if (p == type) return true;
					if (p.type != ValueType::Map) return false;
					d = p.GetDict();
					if (!d.Get(magicIsA, &p)) return false;
				}
			}

			default:
				return false;
		}

	}

	
	double Value::Equality(const Value& lhs, const Value& rhs, int recursionDepth) {
		if (lhs.IsNull()) {
			return rhs.IsNull() ? 1 : 0;
		} else if (lhs.type == ValueType::Number) {
			return (rhs.type == ValueType::Number and lhs.data.number == rhs.data.number) ? 1 : 0;
		} else if (lhs.type == ValueType::String) {
			return (rhs.type == ValueType::String and lhs.GetString() == rhs.GetString()) ? 1 : 0;
		} else if (lhs.type == ValueType::List) {
			if (rhs.type != ValueType::List) return 0;
			const SimpleVector<Value>* lhl = (ValueListStorage*)(lhs.data.ref);
			const SimpleVector<Value>* rhl = (ValueListStorage*)(rhs.data.ref);
			if (lhl == rhl) return 1;	// same data
			if (lhl == nullptr) return rhl == nullptr ? 1 : 0;
			long count = lhl->size();
			if (count != rhl->size()) return 0;
			if (recursionDepth < 1) return 0.5;		// in too deep
			double result = 1;
			for (long i = 0; i < count; i++) {
				result *= Equality((*lhl)[i], (*rhl)[i], recursionDepth-1);
				if (result <= 0) break;
			}
			return result;
		} else if (lhs.type == ValueType::Map) {
			if (rhs.type != ValueType::Map) return 0;
			if (lhs.data.ref == rhs.data.ref) return 1;
			const ValueDict lhd = ((Value)lhs).GetDict();
			const ValueDict rhd = ((Value)rhs).GetDict();
			long count = lhd.Count();
			if (count != rhd.Count()) return 0;
			if (recursionDepth < 1) return 0.5;		// in too deep
			double result = 1;
			Value v;
			for (ValueDictIterator kv = lhd.GetIterator(); not kv.Done(); kv.Next()) {
				if (not rhd.Get(kv.Key(), &v)) return 0;
				result *= Equality(kv.Value(), v, recursionDepth-1);
				if (result <= 0) break;
			}
			return result;
		} else if (lhs.type == ValueType::Function) {
			// Two Function values are equal only if they refer to the exact same function
			if (rhs.type != ValueType::Function) return 0;
			return (lhs.data.ref == rhs.data.ref) ? 1 : 0;
		} else if (lhs.type == ValueType::Temp) {
			return (rhs.type == ValueType::Temp and lhs.data.tempNum == rhs.data.tempNum) ? 1 : 0;
		} else if (lhs.type == ValueType::Var) {
			return (rhs.type == ValueType::Var and lhs.GetString() == rhs.GetString()) ? 1 : 0;
		} else if (lhs.type == ValueType::SeqElem) {
			if (rhs.type != ValueType::SeqElem) return 0;
			SeqElemStorage* lhses = (SeqElemStorage*)lhs.data.ref;
			SeqElemStorage* rhses = (SeqElemStorage*)rhs.data.ref;
			return (lhses->sequence == rhses->sequence and lhses->index == rhses->index) ? 1 : 0;
		}
		return (lhs == rhs) ? 1 : 0;
	}


	/// <summary>
	/// Get the indicated key/value pair as another map containing "key" and "value".
	/// (This is used when iterating over a map with "for".)
	/// </summary>
	/// <param name="index">0-based index of key/value pair to get.</param>
	/// <returns>new map containing "key" and "value" with the requested key/value pair</returns>
	Value Value::GetKeyValuePair(Value map, long index) {
		if (index < 0) throw IndexException(String("index " ) + String::Format(index) + " out of range for map");
		if (map.type != ValueType::Map) return Value::null;
		ValueDict dict = map.GetDict();
		// For now, we'll just iterate from the beginning every time.  This is horribly
		// inefficient on big maps (and not great even on small ones).  OFI: Optimize.
		long i = 0;
		for (ValueDictIterator iter = dict.GetIterator(); !iter.Done(); iter.Next()) {
			if (i == index) {
				// Found the requested entry.  Convert to its own little map.
				ValueDict result;
				result.SetValue(Value::keyString, iter.Key());
				result.SetValue(Value::valueString, iter.Value());
//				dict.forget();
				return Value(result);
			}
			i++;
		}
		// Out of bounds (index too high).
//		dict.forget();
		throw IndexException(String("index " ) + String::Format(index) + " out of range for map");
	}

	unsigned int HashValue(const Value& v) {
		return v.Hash();
	}
	
	unsigned int IntHash(int i) {
		unsigned int x = (unsigned int)i;
		x = ((x >> 16) ^ x) * 0x45d9f3b;
		x = ((x >> 16) ^ x) * 0x45d9f3b;
		x = (x >> 16) ^ x;
		return x;
	}
	
	unsigned int Value::Hash() const {
		switch (type) {
			case ValueType::Null:
				return 0;
				
			case ValueType::Number:
			{
				// Not sure how to hash doubles... for now, we'll just do:
				int i = data.number;
				return IntHash(i);
			}
				
			case ValueType::String:
			case ValueType::Var:
			{
				String temp((StringStorage*)data.ref);
				unsigned int result = temp.Hash();
//				temp.forget();
				return result;
			} break;
				
			case ValueType::List:
			{
				ValueList list((ListStorage<Value>*)data.ref);
				long count = list.Count();
				unsigned int x = IntHash((int)count);
				for (int i=0; i<count; i++) x ^= list[i].Hash();
//				list.forget();
				return x;
			} break;
			
			case ValueType::Map:
			{
				ValueDict dict((DictionaryStorage<Value, Value>*)data.ref);
				long count = dict.Count();
				unsigned int x = IntHash((int)count);
				ValueList keys = dict.Keys();
				for (int i=0; i<count; i++) {
					Value key = keys[i];
					x ^= key.Hash();
					x ^= dict[key].Hash();
				}
//				dict.forget();
				return x;
			} break;

			case ValueType::Temp:
				return IntHash(data.tempNum);
			
			case ValueType::Function:
				return IntHash((int)(long)data.ref);

			case ValueType::SeqElem:
				SeqElemStorage *se = (SeqElemStorage*)data.ref;
				if (!se) return 0;
				return se->index.Hash() ^ se->sequence.Hash();
		}
		return 0;
	}

	bool Value::Equal(StringStorage *lhs, StringStorage *rhs) {
		String a(lhs);
		String b(rhs);
		bool result = (a == b);
//		a.forget();
//		b.forget();
		return result;
	}

	bool Value::Equal(ListStorage<Value> *lhs, ListStorage<Value> *rhs) {
		ValueList a(lhs);
		ValueList b(rhs);
		long count = a.Count();
		bool result = (b.Count() == count);
		if (result) {
			for (long i=0; i<count; i++) if (a[i] != b[i]) { result = false; break; }
		}
//		a.forget();
//		b.forget();
		return result;
	}

	bool Value::Equal(DictionaryStorage<Value, Value> *lhs, DictionaryStorage<Value, Value> *rhs) {
		ValueDict a(lhs);
		ValueDict b(rhs);
		long count = a.Count();
		bool result = (b.Count() == count);
		if (result) {
			List<Value> aKeys = a.Keys();
			for (long i=0; i<count; i++) {
				Value key = aKeys[i];
				Value aval = a[key];
				Value bval;
				if (!b.Get(key, &bval) || aval != bval) {
					result = false;
					break;
				}
			}
		}
//		a.forget();
//		b.forget();
		return result;
	}

	bool Value::Equal(SeqElemStorage *lhs, SeqElemStorage *rhs) {
		return lhs->sequence == rhs->sequence and lhs->index == rhs->index;
	}


//--------------------------------------------------------------------------------
// Unit Tests

class TestValue : public UnitTest
{
public:
	TestValue() : UnitTest("Value") {}
	virtual void Run();
private:
	void TestBasics();
	void TestHashAndEquality();
	void TestSeqElem();
};

void TestValue::Run()
{
	TestBasics();
	TestHashAndEquality();
	TestSeqElem();
}

void TestValue::TestBasics()
{
	Value a(42);
	Value b(a);
	Assert(b.type == ValueType::Number and b.data.number == 42);
	Value c;
	Assert(c.type == ValueType::Null);
	c = b;
	Assert(c.type == ValueType::Number and c.data.number == 42);
	
	a = "Foo!";
	Assert(a.type == ValueType::String and a.ToString() == "Foo!");
	b = a;
	Assert(b.type == ValueType::String and b.ToString() == "Foo!");
	Assert(c.type == ValueType::Number and c.data.number == 42);
	b = 0.0;
	Assert(a.type == ValueType::String and a.ToString() == "Foo!");

	{
		List<Value> lst;
		lst.Add(1);
		lst.Add("two");
		lst.Add(3.14157);
		a = lst;
	}
	Assert(a.type == ValueType::List);
	String s = a.ToString();
	Assert(s == "[1, \"two\", 3.14157]");
}

void TestValue::TestHashAndEquality() {
	Value a(42);
	Value b(42);
	Assert(a.Hash() == b.Hash());
	Assert(a == b);
	Assert(!(a != b));
	b = Value(43);
	Assert(a != b);
	Assert(!(a == b));
	
	a = Value(String("Hello") + " Bob");
	b = String("Hell") + "o Bob";
	Assert(a.Hash() == b.Hash());
	Assert(a == b);
	Assert(!(a != b));
	b = Value("Hello Bill");
	Assert(a != b);
	Assert(!(a == b));

	ValueList lst;
	lst.Add(42);
	lst.Add("foo");
	lst.Add("bar");
	a = lst;
	ValueList lst2;
	lst2.Add(42);
	lst2.Add(String("fo") + "o");
	lst2.Add(String("baroo").Substring(0, 3));
	b = lst;
	Assert(a.Hash() == b.Hash());
	Assert(a == b);
	Assert(!(a != b));

	ValueDict d1;
	d1.SetValue(42, "fourty-two");
	d1.SetValue("foo", "bar");
	a = d1;
	ValueDict d2;
	d2.SetValue("foo", "bar");
	d2.SetValue(42, "fourty-two");
	b = d2;
	Assert(a.Hash() == b.Hash());
	Assert(a == b);
	Assert(!(a != b));
}

void TestValue::TestSeqElem() {
	ValueList lst;
	lst.Add(42);
	lst.Add(Value::one);
	lst.Add("two");
	Value seq = lst;
	Value se = Value::SeqElem(lst, 1);
	Value se2 = Value::SeqElem(lst, Value::zero);

	ValueDict d;
	d.SetValue("foo", "bar");
	Value se3 = Value::SeqElem(d, "foo");
	Value se4 = Value::SeqElem(d, "nosuch");
}
	
RegisterUnitTest(TestValue);
}
