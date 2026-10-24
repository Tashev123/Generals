#include "Core/GenWnd.h"
#include "Core/GenFileSystem.h"
#include "Generals1.h"

namespace
{
	/** The statements of the file: "KEY = value" up to a ';', or one of the bare keywords. */
	void SplitStatements(const FString& Text, TArray<FString>& Out)
	{
		FString Buf;
		bool bQuote = false;
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		for (const FString& RawLine : Lines)
		{
			const FString Line = RawLine.TrimStartAndEnd();
			if (Line.IsEmpty())
			{
				continue;
			}
			if (Buf.IsEmpty() && (Line == TEXT("WINDOW") || Line == TEXT("CHILD") || Line == TEXT("END") || Line == TEXT("ENDALLCHILDREN")
				|| Line == TEXT("STARTLAYOUTBLOCK") || Line == TEXT("ENDLAYOUTBLOCK")))
			{
				Out.Add(Line);
				continue;
			}
			for (TCHAR C : Line)
			{
				if (C == TEXT('"'))
				{
					bQuote = !bQuote;
				}
				if (C == TEXT(';') && !bQuote)
				{
					Out.Add(Buf.TrimStartAndEnd());
					Buf.Reset();
					continue;
				}
				Buf.AppendChar(C);
			}
			if (!Buf.IsEmpty())
			{
				Buf.AppendChar(TEXT(' ')); // (a statement going on over the next line)
			}
		}
	}

	/** Tokens of a value: split on spaces, commas and colons, quotes kept together (without the quotes). */
	void Tokens(const FString& Value, TArray<FString>& Out)
	{
		FString Cur;
		bool bQuote = false;
		auto Flush = [&]() { if (!Cur.IsEmpty()) { Out.Add(Cur); Cur.Reset(); } };
		for (TCHAR C : Value)
		{
			if (C == TEXT('"'))
			{
				if (bQuote) { Out.Add(Cur); Cur.Reset(); }
				else { Flush(); }
				bQuote = !bQuote;
				continue;
			}
			if (!bQuote && (C == TEXT(' ') || C == TEXT('\t') || C == TEXT(',') || C == TEXT(':')))
			{
				Flush();
				continue;
			}
			Cur.AppendChar(C);
		}
		Flush();
	}

	FColor ReadColor(const TArray<FString>& T, int32 i)
	{
		auto At = [&T](int32 k) { return T.IsValidIndex(k) ? uint8(FMath::Clamp(FCString::Atoi(*T[k]), 0, 255)) : uint8(255); };
		return FColor(At(i), At(i + 1), At(i + 2), At(i + 3));
	}

	void ReadDrawData(const FString& Value, TArray<FGenWndDrawData>& Out)
	{
		TArray<FString> T;
		Tokens(Value, T);
		for (int32 i = 0; i < T.Num(); ++i)
		{
			if (T[i] == TEXT("IMAGE") && T.IsValidIndex(i + 1))
			{
				FGenWndDrawData& D = Out.AddDefaulted_GetRef();
				D.Image = T[i + 1].Equals(TEXT("NoImage"), ESearchCase::IgnoreCase) ? FString() : T[i + 1];
				++i;
			}
			else if (T[i] == TEXT("COLOR") && Out.Num() > 0)
			{
				Out.Last().Color = ReadColor(T, i + 1);
				i += 4;
			}
			else if (T[i] == TEXT("BORDERCOLOR") && Out.Num() > 0)
			{
				Out.Last().BorderColor = ReadColor(T, i + 1);
				i += 4;
			}
		}
	}

	void ApplyField(FGenWndWindow& W, const FString& Key, const FString& Value)
	{
		TArray<FString> T;
		if (Key == TEXT("WINDOWTYPE"))
		{
			W.Type = Value.TrimStartAndEnd();
		}
		else if (Key == TEXT("NAME"))
		{
			W.Name = Value.TrimStartAndEnd().TrimQuotes();
			int32 Colon = INDEX_NONE;
			W.ShortName = W.Name.FindLastChar(TEXT(':'), Colon) ? W.Name.Mid(Colon + 1) : W.Name;
		}
		else if (Key == TEXT("SCREENRECT"))
		{
			// UPPERLEFT: x y, BOTTOMRIGHT: x y, CREATIONRESOLUTION: w h
			Tokens(Value, T);
			for (int32 i = 0; i + 2 < T.Num(); ++i)
			{
				const FVector2D P(FCString::Atof(*T[i + 1]), FCString::Atof(*T[i + 2]));
				if (T[i] == TEXT("UPPERLEFT")) { W.Rect.Min = P; }
				else if (T[i] == TEXT("BOTTOMRIGHT")) { W.Rect.Max = P; }
				else if (T[i] == TEXT("CREATIONRESOLUTION")) { W.CreationRes = FIntPoint(int32(P.X), int32(P.Y)); }
			}
			W.Rect.bIsValid = true;
		}
		else if (Key == TEXT("STATUS"))
		{
			Value.TrimStartAndEnd().ParseIntoArray(W.Status, TEXT("+"));
			for (FString& S : W.Status) { S.TrimStartAndEndInline(); }
		}
		else if (Key == TEXT("ENABLEDDRAWDATA")) { ReadDrawData(Value, W.Enabled); }
		else if (Key == TEXT("DISABLEDDRAWDATA")) { ReadDrawData(Value, W.Disabled); }
		else if (Key == TEXT("HILITEDRAWDATA")) { ReadDrawData(Value, W.Hilite); }
		else if (Key == TEXT("TEXT")) { W.Text = Value.TrimStartAndEnd().TrimQuotes(); }
		else if (Key == TEXT("TOOLTIPTEXT")) { W.Tooltip = Value.TrimStartAndEnd().TrimQuotes(); }
		else if (Key == TEXT("FONT"))
		{
			// NAME: "Arial", SIZE: 14, BOLD: 0
			Tokens(Value, T);
			for (int32 i = 0; i + 1 < T.Num(); ++i)
			{
				if (T[i] == TEXT("NAME")) { W.FontName = T[i + 1]; }
				else if (T[i] == TEXT("SIZE")) { W.FontSize = FCString::Atoi(*T[i + 1]); }
				else if (T[i] == TEXT("BOLD")) { W.bBold = FCString::Atoi(*T[i + 1]) != 0; }
			}
		}
		else if (Key == TEXT("TEXTCOLOR"))
		{
			Tokens(Value, T);
			for (int32 i = 0; i + 4 < T.Num(); ++i)
			{
				if (T[i] == TEXT("ENABLED")) { W.TextColor = ReadColor(T, i + 1); }
				else if (T[i] == TEXT("DISABLED")) { W.DisabledTextColor = ReadColor(T, i + 1); }
				else if (T[i] == TEXT("HILITE")) { W.HiliteTextColor = ReadColor(T, i + 1); }
			}
		}
	}

	void CollectInput(const FGenWndWindow& W, TArray<FBox2D>& Out)
	{
		if (W.HasStatus(TEXT("HIDDEN")))
		{
			return;
		}
		if (!W.HasStatus(TEXT("NOINPUT")) && !W.HasStatus(TEXT("SEE_THRU")))
		{
			Out.Add(W.Rect);
		}
		for (const TSharedPtr<FGenWndWindow>& C : W.Children)
		{
			CollectInput(*C, Out);
		}
	}
}

bool FGenWndWindow::HasStatus(const TCHAR* Bit) const
{
	return Status.ContainsByPredicate([Bit](const FString& S) { return S.Equals(Bit, ESearchCase::IgnoreCase); });
}

const FString& FGenWndWindow::GetImage() const
{
	static const FString None;
	return Enabled.Num() > 0 ? Enabled[0].Image : None;
}

const FGenWndWindow* FGenWndWindow::Find(const FString& InShortName) const
{
	if (ShortName.Equals(InShortName, ESearchCase::IgnoreCase))
	{
		return this;
	}
	for (const TSharedPtr<FGenWndWindow>& C : Children)
	{
		if (const FGenWndWindow* F = C->Find(InShortName))
		{
			return F;
		}
	}
	return nullptr;
}

const FGenWndWindow* FGenWndLayout::Find(const FString& ShortName) const
{
	for (const TSharedPtr<FGenWndWindow>& R : Roots)
	{
		if (const FGenWndWindow* F = R->Find(ShortName))
		{
			return F;
		}
	}
	return nullptr;
}

void FGenWndLayout::GetInputRects(TArray<FBox2D>& Out) const
{
	for (const TSharedPtr<FGenWndWindow>& R : Roots)
	{
		CollectInput(*R, Out);
	}
}

TSharedPtr<FGenWndLayout> GenWnd::Parse(const FString& Text, const FString& FileName)
{
	TSharedPtr<FGenWndLayout> Layout = MakeShared<FGenWndLayout>();
	Layout->FileName = FileName;
	TArray<FString> Statements;
	SplitStatements(Text, Statements);
	TArray<FGenWndWindow*> Stack;
	bool bInLayoutBlock = false;
	for (const FString& St : Statements)
	{
		if (St == TEXT("STARTLAYOUTBLOCK")) { bInLayoutBlock = true; continue; }
		if (St == TEXT("ENDLAYOUTBLOCK")) { bInLayoutBlock = false; continue; }
		if (bInLayoutBlock || St == TEXT("CHILD") || St == TEXT("ENDALLCHILDREN"))
		{
			continue;
		}
		if (St == TEXT("WINDOW"))
		{
			TSharedPtr<FGenWndWindow> W = MakeShared<FGenWndWindow>();
			if (Stack.Num() > 0)
			{
				W->Parent = Stack.Last();
				Stack.Last()->Children.Add(W);
			}
			else
			{
				Layout->Roots.Add(W);
			}
			Stack.Add(W.Get());
			continue;
		}
		if (St == TEXT("END"))
		{
			if (Stack.Num() > 0) { Stack.Pop(); }
			continue;
		}
		int32 Eq = INDEX_NONE;
		if (Stack.Num() > 0 && St.FindChar(TEXT('='), Eq))
		{
			ApplyField(*Stack.Last(), St.Left(Eq).TrimStartAndEnd().ToUpper(), St.Mid(Eq + 1));
		}
	}
	return Layout;
}

const FGenWndLayout* GenWnd::Load(const FString& FileName)
{
	static TMap<FString, TSharedPtr<FGenWndLayout>> Cache;
	const FString Key = FileName.ToLower();
	if (const TSharedPtr<FGenWndLayout>* Found = Cache.Find(Key))
	{
		return Found->Get();
	}
	TArray<uint8> Bytes;
	TSharedPtr<FGenWndLayout> Layout;
	if (FGenFileSystem::Get().ReadFile(TEXT("Window\\") + FileName, Bytes))
	{
		// (8-bit text: widened byte by byte.)
		FString Text;
		Text.Reserve(Bytes.Num());
		for (uint8 B : Bytes)
		{
			Text.AppendChar(TCHAR(B));
		}
		Layout = Parse(Text, FileName);
		int32 Count = 0;
		TFunction<void(const FGenWndWindow&)> CountAll = [&Count, &CountAll](const FGenWndWindow& W)
		{
			++Count;
			for (const TSharedPtr<FGenWndWindow>& C : W.Children) { CountAll(*C); }
		};
		for (const TSharedPtr<FGenWndWindow>& R : Layout->Roots) { CountAll(*R); }
		UE_LOG(LogGenerals, Log, TEXT("WND %s: %d top-level window(s), %d in all (first: %s)"), *FileName, Layout->Roots.Num(), Count,
			Layout->Roots.Num() > 0 ? *Layout->Roots[0]->Name : TEXT("-"));
	}
	else
	{
		UE_LOG(LogGenerals, Warning, TEXT("WND '%s' not found"), *FileName);
	}
	Cache.Add(Key, Layout);
	return Layout.Get();
}
