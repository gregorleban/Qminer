#include <base.h>

#include "gtest/gtest.h"

TEST(BlobBs, TBlobBs1) {
    return;

    PBlobBs BlobBs = TGBlobBs::New("./test/cpp/data/test_blobbs.dat", faCreate);

    TMOut MOut; MOut.PutStr("Hello World!");
    TBlobPt BlobPt = BlobBs->PutBlob(MOut.GetSIn());

    TMOut MOut2; MOut2.PutStr("In the tranquil town of Willowbrook, nestled between emerald hills and a gently meandering river, lived a community bound by friendship and tradition. The townspeople, known for their artisanship and devotion to nature, had crafted a life that thrived on simplicity and harmony. Every morning, the faint melodies of birds would greet the dawn, filling the hearts of the residents with a sense of contentment and connection to the world around them. The local bakery, with its heavenly aroma of freshly baked bread, became a symbol of warmth and unity, a place where neighbors would gather to exchange stories and smiles. In Willowbrook, the essence of life was woven into the very fabric of daily existence, creating an atmosphere of peace and joy that lingered long after the sun had set.");
    int ReleasedSize = 0;
    TBlobPt BlobPt2 = BlobBs->PutBlob(BlobPt, MOut2.GetSIn(), ReleasedSize);

    TMOut MOut3; MOut3.PutStr("!dlorW olleH");
    TBlobPt BlobPt3 = BlobBs->PutBlob(MOut3.GetSIn());

    ASSERT_EQ(BlobPt.GetAddr(), BlobPt3.GetAddr());
    ASSERT_EQ(ReleasedSize, 16);
}

TEST(BlobBs, TBlobBs2) {
    return;

    TBlobPt BlobPt, BlobPt2, BlobPt3;
    {
        PBlobBs BlobBs = TGBlobBs::New("./test/cpp/data/test_blobbs.dat", faCreate);
        TMOut MOut; MOut.PutStr("Hello World!");
        BlobPt = BlobBs->PutBlob(MOut.GetSIn());
    }

    {
        PBlobBs BlobBs = TGBlobBs::New("./test/cpp/data/test_blobbs.dat", faUpdate);
        TMOut MOut2; MOut2.PutStr("In the tranquil town of Willowbrook, nestled between emerald hills and a gently meandering river, lived a community bound by friendship and tradition. The townspeople, known for their artisanship and devotion to nature, had crafted a life that thrived on simplicity and harmony. Every morning, the faint melodies of birds would greet the dawn, filling the hearts of the residents with a sense of contentment and connection to the world around them. The local bakery, with its heavenly aroma of freshly baked bread, became a symbol of warmth and unity, a place where neighbors would gather to exchange stories and smiles. In Willowbrook, the essence of life was woven into the very fabric of daily existence, creating an atmosphere of peace and joy that lingered long after the sun had set.");
        int ReleasedSize = 0;
        BlobPt2 = BlobBs->PutBlob(BlobPt, MOut2.GetSIn(), ReleasedSize);
        TMOut MOut3; MOut3.PutStr("!dlorW olleH");
        BlobPt3 = BlobBs->PutBlob(MOut3.GetSIn());

        ASSERT_EQ(ReleasedSize, 16);
    }

    ASSERT_EQ(BlobPt.GetAddr(), BlobPt3.GetAddr());
}

TEST(BlobBs, TBlobBs3) {
    return;

    PBlobBs BlobBs = TMBlobBs::New("./test/cpp/data/test_blobbs.dat", faCreate);

    TMOut MOut; MOut.PutStr("Hello World!");
    TBlobPt BlobPt = BlobBs->PutBlob(MOut.GetSIn());

    TMOut MOut2; MOut2.PutStr("In the tranquil town of Willowbrook, nestled between emerald hills and a gently meandering river, lived a community bound by friendship and tradition. The townspeople, known for their artisanship and devotion to nature, had crafted a life that thrived on simplicity and harmony. Every morning, the faint melodies of birds would greet the dawn, filling the hearts of the residents with a sense of contentment and connection to the world around them. The local bakery, with its heavenly aroma of freshly baked bread, became a symbol of warmth and unity, a place where neighbors would gather to exchange stories and smiles. In Willowbrook, the essence of life was woven into the very fabric of daily existence, creating an atmosphere of peace and joy that lingered long after the sun had set.");
    int ReleasedSize = 0;
    TBlobPt BlobPt2 = BlobBs->PutBlob(BlobPt, MOut2.GetSIn(), ReleasedSize);

    TMOut MOut3; MOut3.PutStr("!dlorW olleH");
    TBlobPt BlobPt3 = BlobBs->PutBlob(MOut3.GetSIn());

    ASSERT_EQ(BlobPt.GetAddr(), BlobPt3.GetAddr());
    ASSERT_EQ(ReleasedSize, 16);
}
